#include "core/grh.hpp"

#include <iostream>
#include <string>

using namespace wolvrix::lib::grh;

namespace
{

    int fail(const std::string &message)
    {
        std::cerr << "[grh-clone-tests] " << message << '\n';
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

    bool compareAttrValues(const AttributeValue &lhs, const AttributeValue &rhs)
    {
        return lhs == rhs;
    }

} // namespace

int main()
{
    try
    {
        Design design;
        Graph &src = design.createGraph("src");
        design.markAsTop("src");

        SymbolId symA = src.internSymbol("a");
        SymbolId symB = src.internSymbol("b");
        SymbolId symSum = src.internSymbol("sum");
        SymbolId symOut = src.internSymbol("out");
        SymbolId symLane1 = src.internSymbol("lane1");
        SymbolId symLane0 = src.internSymbol("lane0");
        SymbolId symPacked = src.internSymbol("packed_lanes");
        SymbolId symPackedIndex = src.internSymbol("packed_index");
        SymbolId symPackedOut = src.internSymbol("packed_out");
        SymbolId symAdd = src.internSymbol("add0");
        SymbolId symAssign = src.internSymbol("assign0");

        ValueId a = src.createValue(symA, 8, false);
        ValueId b = src.createValue(symB, 8, false);
        ValueId sum = src.createValue(symSum, 8, false);
        ValueId out = src.createValue(symOut, 8, false);
        ValueId lane1 = src.createValue(symLane1, 3, false);
        ValueId lane0 = src.createValue(symLane0, 3, false);
        ValueId packed = src.createValue(symPacked, 6, false);
        ValueId packedIndex = src.createValue(symPackedIndex, 1, false);
        ValueId packedOut = src.createValue(symPackedOut, 3, false);
        ValueId ioIn = src.createValue(src.internSymbol("io__in"), 1, false);
        ValueId ioOut = src.createValue(src.internSymbol("io__out"), 1, false);
        ValueId ioOe = src.createValue(src.internSymbol("io__oe"), 1, false);

        src.bindInputPort("a", a);
        src.bindInputPort("b", b);
        src.bindInputPort("lane1", lane1);
        src.bindInputPort("lane0", lane0);
        src.bindInputPort("packed_index", packedIndex);
        src.bindOutputPort("out", out);
        src.bindOutputPort("packed_out", packedOut);
        src.bindInoutPort("io", ioIn, ioOut, ioOe);

        OperationId add = src.createOperation(OperationKind::kAdd, symAdd);
        src.addOperand(add, a);
        src.addOperand(add, b);
        src.addResult(add, sum);
        src.setAttr(add, "delay", AttributeValue(int64_t(3)));
        src.setAttr(add, "label", AttributeValue(std::string("fast")));

        OperationId assign = src.createOperation(OperationKind::kAssign, symAssign);
        src.addOperand(assign, sum);
        src.addResult(assign, out);

        OperationId packedConcat = src.createOperation(OperationKind::kConcat, src.internSymbol("packed_concat"));
        src.addOperand(packedConcat, lane1);
        src.addOperand(packedConcat, lane0);
        src.addResult(packedConcat, packed);
        src.setAttr(packedConcat, "svPackedArray.version", AttributeValue(int64_t(1)));
        src.setAttr(packedConcat, "svPackedArray.elementWidth", AttributeValue(int64_t(3)));
        src.setAttr(packedConcat, "svPackedArray.elementCount", AttributeValue(int64_t(2)));
        src.setAttr(packedConcat, "svPackedArray.indexLow", AttributeValue(int64_t(0)));
        src.setAttr(packedConcat, "svPackedArray.indexHigh", AttributeValue(int64_t(1)));
        src.setAttr(packedConcat, "svPackedArray.indexDirection", AttributeValue(std::string("downto")));
        src.setAttr(packedConcat, "svPackedArray.laneOrder", AttributeValue(std::string("lsb_index_low")));
        src.setAttr(packedConcat, "svPackedArray.concat.operand0Index", AttributeValue(int64_t(1)));
        src.setAttr(packedConcat, "svPackedArray.concat.operandStride", AttributeValue(int64_t(-1)));

        OperationId packedSlice = src.createOperation(OperationKind::kSliceArray, src.internSymbol("packed_slice"));
        src.addOperand(packedSlice, packed);
        src.addOperand(packedSlice, packedIndex);
        src.addResult(packedSlice, packedOut);
        src.setAttr(packedSlice, "sliceWidth", AttributeValue(int64_t(3)));

        SrcLoc opLoc;
        opLoc.file = "clone.sv";
        opLoc.line = 12;
        opLoc.column = 5;
        src.setOpSrcLoc(add, opLoc);

        SrcLoc valLoc;
        valLoc.file = "clone.sv";
        valLoc.line = 8;
        src.setValueSrcLoc(a, valLoc);

        src.addDeclaredSymbol(symA);
        src.addDeclaredSymbol(symSum);

        const SymbolId symGenScope = src.internSymbol("gen_loop");
        const SymbolId symGenName = src.internSymbol("sig");
        const SymbolId symGen0 = src.internSymbol("gen_loop$0$sig");
        const SymbolId symGen1 = src.internSymbol("gen_loop$1$sig");
        src.createValue(symGen0, 8, false);
        src.createValue(symGen1, 8, false);
        src.addDeclaredSymbol(symGen0);
        src.addDeclaredSymbol(symGen1);
        const std::size_t genGroup = src.addGenerateGroup(symGenScope, symGenName);
        src.addGenerateGroupSymbol(genGroup, symGen0);
        src.addGenerateGroupSymbol(genGroup, symGen1);

        Graph &clone = design.cloneGraph("src", "clone");
        if (clone.symbol() != "clone")
        {
            return fail("Clone graph name mismatch");
        }
        if (clone.id() == src.id())
        {
            return fail("Clone graph should have a distinct GraphId");
        }
        for (const auto &name : design.topGraphs())
        {
            if (name == "clone")
            {
                return fail("Clone graph should not be auto-marked as top");
            }
        }

        if (clone.values().size() != src.values().size())
        {
            return fail("Clone value count mismatch");
        }
        if (clone.operations().size() != src.operations().size())
        {
            return fail("Clone operation count mismatch");
        }

        for (const auto srcValueId : src.values())
        {
            Value srcValue = src.getValue(srcValueId);
            std::string_view name = srcValue.symbolText();
            ValueId cloneValueId = clone.findValue(name);
            if (!cloneValueId.valid())
            {
                return fail("Clone missing value: " + std::string(name));
            }
            Value cloneValue = clone.getValue(cloneValueId);
            if (cloneValue.width() != srcValue.width() ||
                cloneValue.isSigned() != srcValue.isSigned() ||
                cloneValue.type() != srcValue.type())
            {
                return fail("Clone value attributes mismatch: " + std::string(name));
            }
            if (cloneValue.isInput() != srcValue.isInput() ||
                cloneValue.isOutput() != srcValue.isOutput() ||
                cloneValue.isInout() != srcValue.isInout())
            {
                return fail("Clone value port flags mismatch: " + std::string(name));
            }
            if (!compareSrcLoc(srcValue.srcLoc(), cloneValue.srcLoc()))
            {
                return fail("Clone value srcLoc mismatch: " + std::string(name));
            }
        }

        for (const auto srcOpId : src.operations())
        {
            Operation srcOp = src.getOperation(srcOpId);
            std::string_view name = srcOp.symbolText();
            OperationId cloneOpId = clone.findOperation(name);
            if (!cloneOpId.valid())
            {
                return fail("Clone missing operation: " + std::string(name));
            }
            Operation cloneOp = clone.getOperation(cloneOpId);
            if (cloneOp.kind() != srcOp.kind())
            {
                return fail("Clone op kind mismatch: " + std::string(name));
            }
            if (!compareSrcLoc(srcOp.srcLoc(), cloneOp.srcLoc()))
            {
                return fail("Clone op srcLoc mismatch: " + std::string(name));
            }
            if (cloneOp.attrs().size() != srcOp.attrs().size())
            {
                return fail("Clone op attr count mismatch: " + std::string(name));
            }
            for (const auto &attr : srcOp.attrs())
            {
                auto cloneAttr = cloneOp.attr(attr.key);
                if (!cloneAttr || !compareAttrValues(*cloneAttr, attr.value))
                {
                    return fail("Clone op attr mismatch: " + std::string(name));
                }
            }
            if (cloneOp.operands().size() != srcOp.operands().size() ||
                cloneOp.results().size() != srcOp.results().size())
            {
                return fail("Clone op arity mismatch: " + std::string(name));
            }
            for (std::size_t i = 0; i < srcOp.operands().size(); ++i)
            {
                std::string_view srcOperandName = src.getValue(srcOp.operands()[i]).symbolText();
                std::string_view cloneOperandName = clone.getValue(cloneOp.operands()[i]).symbolText();
                if (srcOperandName != cloneOperandName)
                {
                    return fail("Clone operand mismatch: " + std::string(name));
                }
            }
            for (std::size_t i = 0; i < srcOp.results().size(); ++i)
            {
                std::string_view srcResultName = src.getValue(srcOp.results()[i]).symbolText();
                std::string_view cloneResultName = clone.getValue(cloneOp.results()[i]).symbolText();
                if (srcResultName != cloneResultName)
                {
                    return fail("Clone result mismatch: " + std::string(name));
                }
            }
        }

        if (clone.inputPorts().size() != src.inputPorts().size() ||
            clone.outputPorts().size() != src.outputPorts().size() ||
            clone.inoutPorts().size() != src.inoutPorts().size())
        {
            return fail("Clone port count mismatch");
        }

        for (const auto &port : src.inputPorts())
        {
            ValueId cloneValue = clone.inputPortValue(port.name);
            if (!cloneValue.valid())
            {
                return fail("Clone missing input port: " + port.name);
            }
            std::string_view srcValueName = src.getValue(port.value).symbolText();
            std::string_view cloneValueName = clone.getValue(cloneValue).symbolText();
            if (srcValueName != cloneValueName)
            {
                return fail("Clone input port value mismatch: " + port.name);
            }
        }

        for (const auto &port : src.outputPorts())
        {
            ValueId cloneValue = clone.outputPortValue(port.name);
            if (!cloneValue.valid())
            {
                return fail("Clone missing output port: " + port.name);
            }
            std::string_view srcValueName = src.getValue(port.value).symbolText();
            std::string_view cloneValueName = clone.getValue(cloneValue).symbolText();
            if (srcValueName != cloneValueName)
            {
                return fail("Clone output port value mismatch: " + port.name);
            }
        }

        for (const auto &port : src.inoutPorts())
        {
            bool found = false;
            for (const auto &clonePort : clone.inoutPorts())
            {
                if (clonePort.name != port.name)
                {
                    continue;
                }
                std::string_view srcIn = src.getValue(port.in).symbolText();
                std::string_view srcOut = src.getValue(port.out).symbolText();
                std::string_view srcOe = src.getValue(port.oe).symbolText();
                std::string_view cloneIn = clone.getValue(clonePort.in).symbolText();
                std::string_view cloneOut = clone.getValue(clonePort.out).symbolText();
                std::string_view cloneOe = clone.getValue(clonePort.oe).symbolText();
                if (srcIn != cloneIn || srcOut != cloneOut || srcOe != cloneOe)
                {
                    return fail("Clone inout port value mismatch: " + port.name);
                }
                found = true;
                break;
            }
            if (!found)
            {
                return fail("Clone missing inout port: " + port.name);
            }
        }

        for (const auto sym : src.declaredSymbols())
        {
            if (!sym.valid())
            {
                continue;
            }
            std::string_view text = src.symbolText(sym);
            if (!clone.isDeclaredSymbol(clone.lookupSymbol(text)))
            {
                return fail("Clone missing declared symbol: " + std::string(text));
            }
        }

        if (clone.generateGroups().size() != src.generateGroups().size())
        {
            return fail("Clone generate group count mismatch");
        }
        for (std::size_t i = 0; i < src.generateGroups().size(); ++i)
        {
            const auto &srcGroup = src.generateGroups()[i];
            const auto &cloneGroup = clone.generateGroups()[i];
            if (clone.symbolText(cloneGroup.scope) != src.symbolText(srcGroup.scope) ||
                clone.symbolText(cloneGroup.name) != src.symbolText(srcGroup.name))
            {
                return fail("Clone generate group scope/name mismatch");
            }
            if (cloneGroup.symbols.size() != srcGroup.symbols.size())
            {
                return fail("Clone generate group member count mismatch");
            }
            for (std::size_t k = 0; k < srcGroup.symbols.size(); ++k)
            {
                if (clone.symbolText(cloneGroup.symbols[k]) != src.symbolText(srcGroup.symbols[k]))
                {
                    return fail("Clone generate group member mismatch");
                }
            }
        }
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception: ") + ex.what());
    }
    return 0;
}
