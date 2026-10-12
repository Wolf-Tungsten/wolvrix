#include "core/grh.hpp"
#include "core/transform.hpp"

#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace wolvrix::lib::grh;
using namespace wolvrix::lib::transform;

namespace
{
    int fail(std::string_view message)
    {
        std::cerr << "[transform-mem-to-reg-selection] " << message << '\n';
        return 1;
    }

    void addMemory(Graph &graph, std::string_view symbol)
    {
        const OperationId memory = graph.createOperation(OperationKind::kMemory,
                                                         graph.internSymbol(std::string(symbol)));
        graph.setAttr(memory, "width", static_cast<int64_t>(8));
        graph.setAttr(memory, "row", static_cast<int64_t>(2));
        graph.setAttr(memory, "isSigned", false);
    }

    bool hasMemory(const Graph &graph, std::string_view symbol)
    {
        for (const auto opId : graph.operations())
        {
            const auto op = graph.getOperation(opId);
            if (op.kind() == OperationKind::kMemory && op.symbolText() == symbol)
            {
                return true;
            }
        }
        return false;
    }

    int runSelectedCase()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        addMemory(graph, "target");
        addMemory(graph, "other");

        const std::vector<std::string_view> args{"-memory-symbol", "target"};
        std::string error;
        auto pass = makePass("mem-to-reg", std::span<const std::string_view>(args), error);
        if (!pass || !error.empty())
        {
            return fail("failed to construct selected mem-to-reg pass: " + error);
        }

        PassManager manager;
        manager.addPass(std::move(pass));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("selected mem-to-reg pass failed");
        }
        if (hasMemory(graph, "target") || !hasMemory(graph, "other"))
        {
            return fail("selection did not convert exactly the requested memory");
        }
        return 0;
    }

    int runDefaultCase()
    {
        Design design;
        Graph &graph = design.createGraph("top");
        addMemory(graph, "first");
        addMemory(graph, "second");

        std::string error;
        const std::vector<std::string_view> args;
        auto pass = makePass("mem-to-reg", std::span<const std::string_view>(args), error);
        if (!pass || !error.empty())
        {
            return fail("failed to construct default mem-to-reg pass: " + error);
        }

        PassManager manager;
        manager.addPass(std::move(pass));
        PassDiagnostics diags;
        const auto result = manager.run(design, diags);
        if (!result.success || diags.hasError())
        {
            return fail("default mem-to-reg pass failed");
        }
        if (hasMemory(graph, "first") || hasMemory(graph, "second"))
        {
            return fail("default behavior should convert all eligible memories");
        }
        return 0;
    }
} // namespace

int main()
{
    if (const int result = runSelectedCase(); result != 0)
    {
        return result;
    }
    return runDefaultCase();
}
