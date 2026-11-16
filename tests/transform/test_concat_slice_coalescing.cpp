#include "core/grh.hpp"
#include "core/transform.hpp"
#include "transform/redundant_elim.hpp"
#include "transform/simplify.hpp"
#include "slang/numeric/SVInt.h"

#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wolvrix::lib;

namespace
{
    struct Piece { unsigned source; int low; int high; };
    struct Case
    {
        std::string name;
        int width;
        std::vector<Piece> pieces;
        std::size_t merged;
    };

    grh::ValueId slice(grh::Graph &g, grh::ValueId source, int low, int high)
    {
        const auto value = g.createValue(high - low + 1, false);
        const auto op = g.createOperation(grh::OperationKind::kSliceStatic);
        g.addOperand(op, source);
        g.addResult(op, value);
        g.setAttr(op, "sliceStart", int64_t(low));
        g.setAttr(op, "sliceEnd", int64_t(high));
        return value;
    }

    slang::SVInt evaluate(const grh::Graph &g, grh::ValueId id,
                         const std::array<slang::SVInt, 2> &inputs)
    {
        const auto value = g.getValue(id);
        slang::SVInt result;
        if (value.isInput())
        {
            result = inputs[value.symbolText() == "x" ? 0 : 1];
        }
        else
        {
            const auto op = g.getOperation(value.definingOp());
            std::vector<slang::SVInt> args;
            for (auto operand : op.operands()) args.push_back(evaluate(g, operand, inputs));
            if (op.kind() == grh::OperationKind::kSliceStatic)
            {
                result = args[0].slice(static_cast<int32_t>(std::get<int64_t>(*op.attr("sliceEnd"))),
                                       static_cast<int32_t>(std::get<int64_t>(*op.attr("sliceStart"))));
            }
            else if (op.kind() == grh::OperationKind::kConcat) result = slang::SVInt::concat(args);
            else if (op.kind() == grh::OperationKind::kAssign) result = args[0];
            else throw std::runtime_error("unexpected test operation");
        }
        result = result.resize(value.width());
        result.setSigned(value.isSigned());
        return result;
    }

    void check(const Case &test, bool isSigned)
    {
        grh::Design design;
        auto &g = design.createGraph("g");
        const std::array sources{g.createValue(g.internSymbol("x"), test.width, isSigned),
                                 g.createValue(g.internSymbol("y"), test.width, isSigned)};
        g.bindInputPort("x", sources[0]);
        g.bindInputPort("y", sources[1]);
        std::vector<grh::ValueId> pieces;
        int width = 0;
        for (const auto &p : test.pieces)
        {
            pieces.push_back(slice(g, sources[p.source], p.low, p.high));
            width += p.high - p.low + 1;
        }
        // Another consumer and an observable name must survive the rewrite.
        g.bindOutputPort("tap", pieces[0]);
        g.addDeclaredSymbol(g.getValue(pieces[0]).symbol());
        const auto result = g.createValue(g.internSymbol("joined"), width, false);
        g.addDeclaredSymbol(g.getValue(result).symbol());
        const auto concat = g.createOperation(grh::OperationKind::kConcat);
        for (auto piece : pieces) g.addOperand(concat, piece);
        g.addResult(concat, result);
        g.bindOutputPort("out", result);
        const auto extended = g.createValue(g.internSymbol("extended"), width + 17, true);
        const auto assign = g.createOperation(grh::OperationKind::kAssign);
        g.addOperand(assign, result);
        g.addResult(assign, extended);
        g.bindOutputPort("extended", extended);

        std::mt19937 rng(20261114);
        std::vector<std::array<slang::SVInt, 2>> inputs;
        std::vector<std::array<slang::SVInt, 3>> expected;
        for (unsigned sample = 0; sample < 256; ++sample)
        {
            std::array<slang::SVInt, 2> values;
            for (auto &v : values)
            {
                std::string literal = std::to_string(test.width) + "'b";
                for (int bit = 0; bit < test.width; ++bit)
                    literal += (sample < 128 ? "01" : "01xz")[rng() % (sample < 128 ? 2 : 4)];
                v = slang::SVInt::fromString(literal);
                v.setSigned(isSigned);
            }
            inputs.push_back(values);
            expected.push_back({evaluate(g, result, values), evaluate(g, pieces[0], values),
                                evaluate(g, extended, values)});
        }
        transform::PassManager manager;
        manager.options().keepDeclaredSymbols = true;
        manager.addPass(std::make_unique<transform::RedundantElimPass>());
        transform::PassDiagnostics diagnostics;
        const auto ran = manager.run(design, diagnostics);
        if (!ran.success || diagnostics.hasError() ||
            g.getOperation(concat).operands().size() != test.merged)
            throw std::runtime_error(test.name + ": wrong concat rewrite");
        if (g.getValue(result).isSigned() || !g.isDeclaredSymbol(g.getValue(result).symbol()) ||
            !g.getValue(pieces[0]).isOutput())
            throw std::runtime_error(test.name + ": observable metadata changed");
        // Exercise simplification to a fixed point, including DCE and use lists.
        transform::PassManager simplify;
        simplify.options().keepDeclaredSymbols = true;
        simplify.addPass(std::make_unique<transform::SimplifyPass>());
        if (!simplify.run(design, diagnostics).success || diagnostics.hasError())
            throw std::runtime_error(test.name + ": simplify failed");
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const std::array observed{evaluate(g, result, inputs[i]), evaluate(g, pieces[0], inputs[i]),
                                      evaluate(g, extended, inputs[i])};
            for (unsigned port = 0; port < observed.size(); ++port)
                if (!exactlyEqual(observed[port], expected[i][port]))
                    throw std::runtime_error(test.name + ": two/four-state value mismatch");
        }
        if (simplify.run(design, diagnostics).changed)
            throw std::runtime_error(test.name + ": simplify did not converge");
    }

    void invalidRanges()
    {
        // Invalid/out-of-range slices are not concatenated into a valid source.
        for (unsigned mode = 0; mode < 5; ++mode)
        {
            grh::Design design;
            auto &g = design.createGraph("invalid");
            const auto x = g.createValue(g.internSymbol("x"), 8, false);
            g.bindInputPort("x", x);
            const auto hi = slice(g, x, 4, 7);
            const auto lo = slice(g, x, 0, 3);
            const auto def = g.getValue(hi).definingOp();
            if (mode == 0) g.setAttr(def, "sliceEnd", int64_t(8));
            if (mode == 1) g.setAttr(def, "sliceStart", int64_t(-1));
            if (mode == 2) g.setAttr(def, "sliceEnd", std::string("7"));
            if (mode == 3) g.setAttr(def, "sliceEnd", int64_t(6));
            const auto output = g.createValue(g.internSymbol("out"), mode == 4 ? 9 : 8, false);
            const auto concat = g.createOperation(grh::OperationKind::kConcat);
            g.addOperand(concat, hi); g.addOperand(concat, lo); g.addResult(concat, output);
            g.bindOutputPort("out", output);
            transform::PassManager manager;
            manager.addPass(std::make_unique<transform::RedundantElimPass>());
            transform::PassDiagnostics diagnostics;
            if (!manager.run(design, diagnostics).success || diagnostics.hasError() ||
                g.getOperation(concat).operands().size() != 2)
                throw std::runtime_error("malformed range/width must not merge");
        }
    }
}

int main()
{
    try
    {
        std::vector<Piece> bits;
        for (int i = 31; i >= 0; --i) bits.push_back({0, i, i});
        const std::vector<Case> cases{
            {"whole bits", 32, bits, 1},
            {"wide whole", 129, {{0,97,128},{0,65,96},{0,33,64},{0,0,32}}, 1},
            {"partial cross-word", 65, {{0,40,64},{0,20,39},{0,9,19}}, 1},
            {"two sources", 32, {{0,8,15},{0,0,7},{1,16,31},{1,0,15}}, 2},
            {"gap", 32, {{0,24,31},{0,8,15},{0,0,7}}, 2},
            {"reversed", 32, {{0,0,7},{0,8,15},{0,16,23}}, 3},
            {"overlap", 32, {{0,8,15},{0,0,9}}, 2},
            {"duplicate", 32, {{0,0,7},{0,0,7}}, 2},
            {"different source", 32, {{0,8,15},{1,0,7}}, 2}
        };
        for (const auto &test : cases)
            for (bool isSigned : {false, true}) check(test, isSigned);
        invalidRanges();
        std::cout << "concat slice coalescing: signed/unsigned, two/four-state and negative cases passed\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
