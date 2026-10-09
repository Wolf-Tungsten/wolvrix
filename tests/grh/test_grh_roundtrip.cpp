#include "core/grh.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>

using namespace wolvrix::lib::grh;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grh_roundtrip_tests] " << message << '\n';
        return 1;
    }

    bool compareSrcLoc(const std::optional<SrcLoc> &lhs, const std::optional<SrcLoc> &rhs)
    {
        if (lhs.has_value() != rhs.has_value())
        {
            return false;
        }
        if (!lhs)
        {
            return true;
        }
        return lhs->file == rhs->file &&
               lhs->line == rhs->line &&
               lhs->column == rhs->column &&
               lhs->endLine == rhs->endLine &&
               lhs->endColumn == rhs->endColumn &&
               lhs->origin == rhs->origin &&
               lhs->pass == rhs->pass &&
               lhs->note == rhs->note;
    }

    std::string compareViews(const GraphView &lhs, const GraphView &rhs)
    {
        auto lhsValues = lhs.values();
        auto rhsValues = rhs.values();
        if (lhsValues.size() != rhsValues.size())
        {
            return "value count mismatch";
        }
        for (std::size_t i = 0; i < lhsValues.size(); ++i)
        {
            if (lhsValues[i] != rhsValues[i])
            {
                return "value order mismatch";
            }
        }

        auto lhsOps = lhs.operations();
        auto rhsOps = rhs.operations();
        if (lhsOps.size() != rhsOps.size())
        {
            return "operation count mismatch";
        }
        for (std::size_t i = 0; i < lhsOps.size(); ++i)
        {
            if (lhsOps[i] != rhsOps[i])
            {
                return "operation order mismatch";
            }
        }

        auto lhsInPorts = lhs.inputPorts();
        auto rhsInPorts = rhs.inputPorts();
        if (lhsInPorts.size() != rhsInPorts.size())
        {
            return "input port count mismatch";
        }
        for (std::size_t i = 0; i < lhsInPorts.size(); ++i)
        {
            if (lhsInPorts[i].name != rhsInPorts[i].name || lhsInPorts[i].value != rhsInPorts[i].value)
            {
                return "input port mismatch";
            }
        }

        auto lhsOutPorts = lhs.outputPorts();
        auto rhsOutPorts = rhs.outputPorts();
        if (lhsOutPorts.size() != rhsOutPorts.size())
        {
            return "output port count mismatch";
        }
        for (std::size_t i = 0; i < lhsOutPorts.size(); ++i)
        {
            if (lhsOutPorts[i].name != rhsOutPorts[i].name || lhsOutPorts[i].value != rhsOutPorts[i].value)
            {
                return "output port mismatch";
            }
        }

        auto lhsInoutPorts = lhs.inoutPorts();
        auto rhsInoutPorts = rhs.inoutPorts();
        if (lhsInoutPorts.size() != rhsInoutPorts.size())
        {
            return "inout port count mismatch";
        }
        for (std::size_t i = 0; i < lhsInoutPorts.size(); ++i)
        {
            if (lhsInoutPorts[i].name != rhsInoutPorts[i].name ||
                lhsInoutPorts[i].in != rhsInoutPorts[i].in ||
                lhsInoutPorts[i].out != rhsInoutPorts[i].out ||
                lhsInoutPorts[i].oe != rhsInoutPorts[i].oe)
            {
                return "inout port mismatch";
            }
        }

        for (std::size_t i = 0; i < lhsOps.size(); ++i)
        {
            auto opId = lhsOps[i];
            if (lhs.opKind(opId) != rhs.opKind(opId))
            {
                return "opKind mismatch";
            }
            if (lhs.opSymbol(opId) != rhs.opSymbol(opId))
            {
                return "opSymbol mismatch";
            }
            if (!compareSrcLoc(lhs.opSrcLoc(opId), rhs.opSrcLoc(opId)))
            {
                return "opSrcLoc mismatch";
            }

            auto lhsOperands = lhs.opOperands(opId);
            auto rhsOperands = rhs.opOperands(opId);
            if (lhsOperands.size() != rhsOperands.size())
            {
                return "opOperands size mismatch";
            }
            for (std::size_t j = 0; j < lhsOperands.size(); ++j)
            {
                if (lhsOperands[j] != rhsOperands[j])
                {
                    return "opOperands mismatch";
                }
            }

            auto lhsResults = lhs.opResults(opId);
            auto rhsResults = rhs.opResults(opId);
            if (lhsResults.size() != rhsResults.size())
            {
                return "opResults size mismatch";
            }
            for (std::size_t j = 0; j < lhsResults.size(); ++j)
            {
                if (lhsResults[j] != rhsResults[j])
                {
                    return "opResults mismatch";
                }
            }

            auto lhsAttrs = lhs.opAttrs(opId);
            auto rhsAttrs = rhs.opAttrs(opId);
            if (lhsAttrs.size() != rhsAttrs.size())
            {
                return "opAttrs size mismatch";
            }
            for (std::size_t j = 0; j < lhsAttrs.size(); ++j)
            {
                if (lhsAttrs[j].key != rhsAttrs[j].key || lhsAttrs[j].value != rhsAttrs[j].value)
                {
                    return "opAttrs mismatch";
                }
            }
        }

        for (std::size_t i = 0; i < lhsValues.size(); ++i)
        {
            auto valueId = lhsValues[i];
            if (lhs.valueSymbol(valueId) != rhs.valueSymbol(valueId))
            {
                return "valueSymbol mismatch";
            }
            if (lhs.valueWidth(valueId) != rhs.valueWidth(valueId))
            {
                return "valueWidth mismatch";
            }
            if (lhs.valueSigned(valueId) != rhs.valueSigned(valueId))
            {
                return "valueSigned mismatch";
            }
            auto lhsShape = lhs.valueShape(valueId);
            auto rhsShape = rhs.valueShape(valueId);
            if (lhsShape.size() != rhsShape.size() ||
                !std::equal(lhsShape.begin(), lhsShape.end(), rhsShape.begin()))
            {
                return "valueShape mismatch";
            }
            if (lhs.valueIsInput(valueId) != rhs.valueIsInput(valueId))
            {
                return "valueIsInput mismatch";
            }
            if (lhs.valueIsOutput(valueId) != rhs.valueIsOutput(valueId))
            {
                return "valueIsOutput mismatch";
            }
            if (lhs.valueIsInout(valueId) != rhs.valueIsInout(valueId))
            {
                return "valueIsInout mismatch";
            }
            if (lhs.valueDef(valueId) != rhs.valueDef(valueId))
            {
                return "valueDef mismatch";
            }
            if (!compareSrcLoc(lhs.valueSrcLoc(valueId), rhs.valueSrcLoc(valueId)))
            {
                return "valueSrcLoc mismatch";
            }

            auto lhsUsers = lhs.valueUsers(valueId);
            auto rhsUsers = rhs.valueUsers(valueId);
            if (lhsUsers.size() != rhsUsers.size())
            {
                return "valueUsers size mismatch";
            }
            for (std::size_t j = 0; j < lhsUsers.size(); ++j)
            {
                if (lhsUsers[j].operation != rhsUsers[j].operation ||
                    lhsUsers[j].operandIndex != rhsUsers[j].operandIndex)
                {
                    return "valueUsers mismatch";
                }
            }
        }

        return "";
    }

} // namespace

int main()
{
    try
    {
        namespace grh_ir = wolvrix::lib::grh;

        grh_ir::GraphSymbolTable graphSymbols;
        grh_ir::GraphBuilder builder(graphSymbols);

        auto symA = graphSymbols.intern("a");
        auto symB = graphSymbols.intern("b");
        auto symTmp = graphSymbols.intern("tmp");
        auto symOutVal = graphSymbols.intern("out_val");
        auto symArr = graphSymbols.intern("arr");
        auto symArr2 = graphSymbols.intern("arr2");
        auto symAdd = graphSymbols.intern("add0");
        auto symAssign = graphSymbols.intern("assign0");

        auto vA = builder.addValue(symA, 8, false);
        auto vB = builder.addValue(symB, 8, false);
        auto vTmp = builder.addValue(symTmp, 8, true);
        auto vOut = builder.addValue(symOutVal, 8, false);
        const std::vector<int64_t> arrShape{4};
        const std::vector<int64_t> arr2Shape{2, 4};
        auto vArr = builder.addValue(symArr, 32, false, ValueType::Logic, arrShape);
        auto vArr2 = builder.addValue(symArr2, 64, true, ValueType::Logic, arr2Shape);
        (void)vArr;
        (void)vArr2;

        auto opAdd = builder.addOp(OperationKind::kAdd, symAdd);
        builder.addOperand(opAdd, vA);
        builder.addOperand(opAdd, vB);
        builder.addResult(opAdd, vTmp);

        auto opAssign = builder.addOp(OperationKind::kAssign, symAssign);
        builder.addOperand(opAssign, vTmp);
        builder.addResult(opAssign, vOut);

        builder.bindInputPort("in_a", vA);
        builder.bindInputPort("in_b", vB);
        builder.bindOutputPort("out", vOut);

        builder.setAttr(opAdd, "delay", AttributeValue(int64_t(3)));
        builder.setAttr(opAdd, "label", AttributeValue(std::string("fast")));

        SrcLoc opLoc;
        opLoc.file = "roundtrip.sv";
        opLoc.line = 21;
        opLoc.column = 4;
        builder.setOpSrcLoc(opAdd, opLoc);

        SrcLoc valLoc;
        valLoc.file = "roundtrip.sv";
        valLoc.line = 22;
        valLoc.column = 1;
        builder.setValueSrcLoc(vA, valLoc);

        grh_ir::GraphView view = builder.freeze();
        grh_ir::GraphBuilder rebuilt = grh_ir::GraphBuilder::fromView(view, graphSymbols);
        grh_ir::GraphView roundTrip = rebuilt.freeze();

        const std::string mismatch = compareViews(view, roundTrip);
        if (!mismatch.empty())
        {
            return fail(std::string("GraphBuilder fromView roundtrip ") + mismatch);
        }
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception: ") + ex.what());
    }
    return 0;
}
