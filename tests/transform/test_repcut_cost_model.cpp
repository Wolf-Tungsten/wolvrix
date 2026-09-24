#include "transform/repcut_cost_model.hpp"
#include "transform/repcut.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib;
using namespace wolvrix::lib::transform;

namespace
{
    struct Fixture
    {
        grh::Design design;
        grh::Graph &graph = design.createGraph("cost_fixture");
        unsigned serial = 0;

        grh::OperationId operation(grh::OperationKind kind, std::vector<int32_t> operands,
                                   int32_t resultWidth)
        {
            const auto id = graph.createOperation(kind, graph.internSymbol("op" + std::to_string(serial++)));
            for (const auto width : operands)
            {
                const auto value = graph.createValue(graph.internSymbol("v" + std::to_string(serial++)), width, false);
                graph.addOperand(id, value);
            }
            if (resultWidth > 0)
                graph.addResult(id, graph.createValue(graph.internSymbol("v" + std::to_string(serial++)), resultWidth, false));
            return id;
        }

        RepcutOperationCost cost(grh::OperationKind kind, std::vector<int32_t> operands,
                                 int32_t resultWidth, RepcutCostModel model = RepcutCostModel::kVerilatorV1)
        {
            const auto id = operation(kind, operands, resultWidth);
            return estimateRepcutOperationCost(graph, graph.getOperation(id), model);
        }

        uint32_t memoryMaskCost(std::string literal,
                                RepcutCostModel model = RepcutCostModel::kVerilatorV1)
        {
            const auto id = operation(grh::OperationKind::kMemoryWritePort, {1, 8, 256, 256}, 0);
            const auto mask = graph.getOperation(id).operands()[3];
            const auto constant = graph.createOperation(grh::OperationKind::kConstant,
                                                        graph.internSymbol("mask" + std::to_string(serial++)));
            graph.setAttr(constant, "constValue", std::move(literal));
            graph.addResult(constant, mask);
            return estimateRepcutOperationCost(graph, graph.getOperation(id), model).weight;
        }
    };

    void check(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        using K = grh::OperationKind;
        Fixture f;
        check(RepcutOptions{}.costModel == RepcutCostModel::kLegacy, "default must stay legacy");
        check(repcutCostModelName(RepcutCostModel::kVerilatorV1) == "verilator-v1", "versioned name");
        for (const auto width : {32, 64, 65, 128, 256, 1024})
        {
            const auto words = width <= 64 ? 1u : (width + 31u) / 32u;
            const auto add = f.cost(K::kAdd, {width, width}, width);
            const auto reduction = f.cost(K::kReduceOr, {width}, 1);
            const auto slice = f.cost(K::kSliceStatic, {width}, 8);
            const auto dynamic = f.cost(K::kSliceDynamic, {width, 32}, 8);
            check(add.weight == 2 * words && !add.usedLegacyFallback, "add word scaling");
            check(reduction.weight == words, "reduction must use input width");
            check(slice.weight == 1 && dynamic.weight == 4, "narrow slices do not scan all source words");
            check(f.cost(K::kLogicNot, {width}, 1).weight == words, "wide boolean conversion");
            check(f.cost(K::kEq, {width, width}, 1).weight == 2 * words, "comparison input width");
            check(f.cost(K::kShl, {width, 16}, width).weight == 2 * words, "wide shift output width");
            check(f.cost(K::kAdd, {width, width}, width, RepcutCostModel::kLegacy).weight == 2,
                  "legacy add golden weight");
            check(f.cost(K::kReduceOr, {width}, 1, RepcutCostModel::kLegacy).weight == 1,
                  "legacy reduction golden weight");
            std::cout << "width=" << width << " add=" << add.weight << " reduce_or=" << reduction.weight
                      << " slice8=" << slice.weight << " dynamic_slice8=" << dynamic.weight << '\n';
        }
        check(f.cost(K::kMemoryWritePort, {1, 8, 256, 256}, 0).weight == 513,
              "dynamic memory mask per-bit rule/data operand 2");
        check(f.cost(K::kMemoryWritePort, {1, 64, 256, 256}, 0).weight == 513,
              "address width must not replace data width");
        const auto registerWrite = f.cost(K::kRegisterWritePort, {1, 256, 256, 1}, 0);
        check(registerWrite.weight == 5 && registerWrite.usedLegacyFallback,
              "uncalibrated partial register writes fall back using data operand 1");
        check(f.memoryMaskCost("256'h0") == 1, "zero memory mask");
        check(f.memoryMaskCost("'0") == 1, "unbased zero memory mask");
        check(f.memoryMaskCost("'1") == 513, "unbased one retains conservative mask rule");
        check(f.memoryMaskCost("256'hx") == 513, "unknown mask stays conservative");
        check(f.memoryMaskCost("256'hffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff") == 9,
              "whole-row memory write");
        check(f.memoryMaskCost("256'h5") == 3, "partial memory mask counts enabled bits");
        for (const auto kind : {K::kMul, K::kDiv, K::kMod, K::kDpicCall, K::kMemoryReadPort})
        {
            const auto id = f.operation(kind, {256, 256}, 256);
            const auto op = f.graph.getOperation(id);
            const auto candidate = estimateRepcutOperationCost(f.graph, op, RepcutCostModel::kVerilatorV1);
            check(candidate.usedLegacyFallback && candidate.weight ==
                  estimateRepcutOperationCost(f.graph, op, RepcutCostModel::kLegacy).weight, "explicit fallback");
        }
        check(f.cost(K::kMemoryWritePort, {1, 8, std::numeric_limits<int32_t>::max(), 1}, 0).weight ==
              std::numeric_limits<uint32_t>::max() - 1u, "saturation must not alias cache sentinel");
        const auto invalid = static_cast<RepcutCostModel>(999);
        check(repcutCostModelName(invalid) == "unknown", "unknown model name");
        bool rejected = false;
        try { (void)f.cost(K::kAdd, {8, 8}, 8, invalid); }
        catch (const std::invalid_argument &) { rejected = true; }
        check(rejected, "unknown model must not silently fall back");
        for (const auto token : {"legacy", "verilator-v1"})
        {
            std::string error;
            const std::string option = std::string("-cost-model=") + token;
            check(makePass("repcut", std::vector<std::string_view>{option}, error) != nullptr, "equals cost-model option");
            check(makePass("repcut", std::vector<std::string_view>{"-cost-model", token, "-weight-mode", "closure-aware"}, error) != nullptr,
                  "orthogonal separated option");
        }
        for (const auto token : {"", "gsim", "grhsim", "VERILATOR-V1", "verilator"})
        {
            std::string error;
            check(makePass("repcut", std::vector<std::string_view>{"-cost-model", token}, error) == nullptr, "strict model validation");
        }
        std::string error;
        check(makePass("repcut", std::vector<std::string_view>{"-cost-model"}, error) == nullptr, "missing model value");
    }
    catch (const std::exception &error)
    {
        std::cerr << "[repcut-cost-model] " << error.what() << '\n';
        return 1;
    }
    return 0;
}
