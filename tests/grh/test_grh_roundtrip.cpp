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

    int borrowedSourceLocations()
    {
        Design design;
        Graph& graph = design.createGraph("borrowed_locations");
        const auto op = graph.createOperation(OperationKind::kConstant, graph.internSymbol("op"));
        const auto value = graph.createValue(graph.internSymbol("value"), 8, false);
        graph.addResult(op, value);
        const SrcLoc expected{std::string(2048, 'f'), 5, 6, 7, 8,
            std::string(1024, 'o'), std::string(1024, 'p'), std::string(4096, 'n')};
        {
            SrcLoc temporary = expected;
            graph.setOpAndValueSrcLocView(op, value, SrcLocView(temporary));
            temporary.file.assign(temporary.file.size(), 'x');
            temporary.note.clear();
        }
        if (!compareSrcLoc(graph.opSrcLoc(op), expected) ||
            !compareSrcLoc(graph.valueSrcLoc(value), expected) || graph.payloadStats().srcLocs != 1)
            return fail("borrowed source location was not owned and shared by both entities");

        graph.setOpSrcLocView(op, SrcLocView(expected));
        graph.setValueSrcLocView(value, SrcLocView(expected));
        if (graph.payloadStats().srcLocs != 1)
            return fail("borrowed single-entity setters did not deduplicate the source record");

        const SrcLoc replacement{"replacement.sv", 11};
        const auto rejects = [](auto&& write) {
            try { write(); }
            catch (const std::exception&) { return true; }
            return false;
        };
        const auto erasedOp = graph.createOperation(OperationKind::kConstant);
        const auto erasedValue = graph.createValue(8, false);
        graph.eraseOp(erasedOp);
        graph.eraseValue(erasedValue);
        Graph& foreign = design.createGraph("foreign_locations");
        const auto foreignOp = foreign.createOperation(OperationKind::kConstant);
        const auto foreignValue = foreign.createValue(8, false);
        for (const auto invalid : {OperationId::invalid(), erasedOp, foreignOp}) {
            if (!rejects([&] { graph.setOpAndValueSrcLocView(invalid, value, SrcLocView(replacement)); }) ||
                !compareSrcLoc(graph.valueSrcLoc(value), expected))
                return fail("invalid paired source-location operation changed the value");
        }
        for (const auto invalid : {ValueId::invalid(), erasedValue, foreignValue}) {
            if (!rejects([&] { graph.setOpAndValueSrcLocView(op, invalid, SrcLocView(replacement)); }) ||
                !compareSrcLoc(graph.opSrcLoc(op), expected))
                return fail("invalid paired source-location value changed the operation");
        }
        graph.compact();
        const auto compactOp = graph.findOperation("op");
        const auto compactValue = graph.findValue("value");
        if (!compareSrcLoc(graph.opSrcLoc(compactOp), expected) ||
            !compareSrcLoc(graph.valueSrcLoc(compactValue), expected))
            return fail("compact changed borrowed source locations");
        graph.setOpAndValueSrcLocView(compactOp, compactValue, SrcLocView{});
        if (!compareSrcLoc(graph.opSrcLoc(compactOp), SrcLoc{}) ||
            !compareSrcLoc(graph.valueSrcLoc(compactValue), SrcLoc{}))
            return fail("empty borrowed source location changed the owning setter semantics");
        return 0;
    }

    int copiedSourceLocations()
    {
        const SrcLoc expected{std::string(8192, 'f'), 123, 45, 678, 90,
            std::string(4096, 'o'), std::string(2048, 'p'), std::string(16384, 'n')};
        Design destination;
        Graph& target = destination.createGraph("destination");
        auto targetOp = target.createOperation(OperationKind::kConstant, target.internSymbol("imported_op"));
        auto targetValue = target.createValue(target.internSymbol("imported_value"), 8, false);
        auto copiedOp = target.createOperation(OperationKind::kConstant, target.internSymbol("copied_op"));
        auto copiedValue = target.createValue(target.internSymbol("copied_value"), 8, false);
        {
            Design origin;
            Graph& source = origin.createGraph("source");
            const auto erasedOp = source.createOperation(OperationKind::kConstant, source.internSymbol("erased_op"));
            const auto erasedValue = source.createValue(source.internSymbol("erased_value"), 8, false);
            auto sourceOp = source.createOperation(OperationKind::kConstant, source.internSymbol("source_op"));
            auto sourceValue = source.createValue(source.internSymbol("source_value"), 8, false);
            const auto emptyOp = source.createOperation(OperationKind::kConstant);
            const auto emptyValue = source.createValue(8, false);
            source.setOpSrcLoc(sourceOp, expected);
            source.setValueSrcLoc(sourceValue, expected);
            if (!source.eraseOp(erasedOp) || !source.eraseValue(erasedValue))
                return fail("source-location fixture did not create tombstones");

            target.setOpSrcLoc(targetOp, SrcLoc{"replaced.sv", 1});
            target.setValueSrcLoc(targetValue, SrcLoc{"replaced.sv", 1});
            target.copyOpSrcLoc(targetOp, source, sourceOp);
            target.copyValueSrcLoc(targetValue, source, sourceValue);
            if (!compareSrcLoc(target.opSrcLoc(targetOp), expected) ||
                !compareSrcLoc(target.valueSrcLoc(targetValue), expected))
                return fail("cross-graph source-location import changed strings or coordinates");

            const auto records = target.payloadStats().srcLocs;
            target.copyOpSrcLoc(copiedOp, target, targetOp);
            target.copyValueSrcLoc(copiedValue, target, targetValue);
            target.copyOpSrcLoc(targetOp, target, targetOp);
            target.copyValueSrcLoc(targetValue, target, targetValue);
            if (target.payloadStats().srcLocs != records ||
                !compareSrcLoc(target.opSrcLoc(copiedOp), expected) ||
                !compareSrcLoc(target.valueSrcLoc(copiedValue), expected))
                return fail("same-graph source-location copy did not reuse its pooled record");

            target.copyOpSrcLoc(copiedOp, source, emptyOp);
            target.copyValueSrcLoc(copiedValue, source, emptyValue);
            if (target.opSrcLoc(copiedOp) || target.valueSrcLoc(copiedValue))
                return fail("copying an absent source location did not clear the destination");
            target.copyOpSrcLoc(copiedOp, target, targetOp);
            target.copyValueSrcLoc(copiedValue, target, targetValue);
            const auto localEmptyOp = target.createOperation(OperationKind::kConstant);
            const auto localEmptyValue = target.createValue(8, false);
            target.copyOpSrcLoc(copiedOp, target, localEmptyOp);
            target.copyValueSrcLoc(copiedValue, target, localEmptyValue);
            if (target.opSrcLoc(copiedOp) || target.valueSrcLoc(copiedValue))
                return fail("same-graph absent source location did not clear the destination");

            Graph& foreign = origin.createGraph("foreign");
            const auto foreignOp = foreign.createOperation(OperationKind::kConstant);
            const auto foreignValue = foreign.createValue(8, false);
            const auto rejects = [](auto&& copy) {
                try { copy(); }
                catch (const std::exception&) { return true; }
                return false;
            };
            for (const auto invalid : {OperationId::invalid(), erasedOp, foreignOp}) {
                if (!rejects([&] { target.copyOpSrcLoc(targetOp, source, invalid); }) ||
                    !compareSrcLoc(target.opSrcLoc(targetOp), expected))
                    return fail("invalid or erased source operation changed destination provenance");
            }
            for (const auto invalid : {ValueId::invalid(), erasedValue, foreignValue}) {
                if (!rejects([&] { target.copyValueSrcLoc(targetValue, source, invalid); }) ||
                    !compareSrcLoc(target.valueSrcLoc(targetValue), expected))
                    return fail("invalid or erased source value changed destination provenance");
            }

            source.compact();
            target.compact();
            sourceOp = source.findOperation("source_op");
            sourceValue = source.findValue("source_value");
            targetOp = target.findOperation("imported_op");
            targetValue = target.findValue("imported_value");
            copiedOp = target.findOperation("copied_op");
            copiedValue = target.findValue("copied_value");
            target.copyOpSrcLoc(copiedOp, source, sourceOp);
            target.copyValueSrcLoc(copiedValue, source, sourceValue);
            if (target.payloadStats().srcLocs != 1 ||
                !compareSrcLoc(target.opSrcLoc(copiedOp), expected) ||
                !compareSrcLoc(target.valueSrcLoc(copiedValue), expected))
                return fail("source-location copying after compact changed provenance or deduplication");
        }
        target.compact();
        for (const auto name : {"imported_op", "copied_op"})
            if (!compareSrcLoc(target.opSrcLoc(target.findOperation(name)), expected))
                return fail("imported operation provenance borrowed destroyed source strings");
        for (const auto name : {"imported_value", "copied_value"})
            if (!compareSrcLoc(target.valueSrcLoc(target.findValue(name)), expected))
                return fail("imported value provenance borrowed destroyed source strings");
        return 0;
    }

    int payloadFirstSymbols()
    {
        const std::string promoted(4096, 'a');
        const std::string deferred(4096, 'b');
        const std::string cloneOnly(4096, 'c');
        Design cloned;
        {
            Design design;
            Graph& graph = design.createGraph("payload_first");
            auto holder = graph.createOperation(OperationKind::kConstant, graph.internSymbol("holder"));
            for (int index = 0; index < 32; ++index)
                graph.setAttr(holder, "dynamic-key-" + std::to_string(index),
                    "discarded-payload-" + std::to_string(index));
            graph.setAttr(holder, "promoted", promoted);
            graph.setAttr(holder, "deferred", deferred);
            graph.setAttr(holder, "clone_only", cloneOnly);
            uint32_t payloadId = 0;
            for (uint32_t id = 1; id <= graph.payloadStats().strings; ++id)
                if (graph.payloadStore().string(id) == promoted) payloadId = id;
            if (!payloadId || graph.lookupSymbol(promoted).valid())
                return fail("payload-first text was prematurely registered as a symbol");
            const auto promotedSymbol = graph.internSymbol(promoted);
            if (!promotedSymbol.valid() || promotedSymbol.value == payloadId ||
                graph.lookupSymbol(promoted) != promotedSymbol ||
                graph.symbolText(promotedSymbol).data() !=
                    graph.opAttrRange(holder).find("promoted")->value().asString().data())
                return fail("payload promotion conflated StringId with SymbolId or duplicated text bytes");
            graph.createValue(promotedSymbol, 8, false);
            for (int index = 0; index < 32; ++index)
                graph.eraseAttr(holder, "dynamic-key-" + std::to_string(index));
            graph.compact();
            holder = graph.findOperation("holder");
            if (graph.lookupSymbol(promoted) != promotedSymbol || graph.lookupSymbol(deferred).valid())
                return fail("compact changed symbol registration of payload-first text");
            const auto deferredSymbol = graph.internSymbol(deferred);
            if (!deferredSymbol.valid() || deferredSymbol.value != promotedSymbol.value + 1 ||
                graph.symbolText(deferredSymbol).data() !=
                    graph.opAttrRange(holder).find("deferred")->value().asString().data())
                return fail("payload promotion after compact changed symbol numbering or shared text");
            graph.createValue(deferredSymbol, 8, false);

            cloned = design.clone();
            Graph& clone = *cloned.findGraph("payload_first");
            const auto cloneHolder = clone.findOperation("holder");
            if (!clone.findValue(promoted).valid() || !clone.findValue(deferred).valid() ||
                clone.lookupSymbol(cloneOnly).valid())
                return fail("clone changed promoted or payload-only symbol registration");
            const auto cloneSymbol = clone.internSymbol(cloneOnly);
            clone.createValue(cloneSymbol, 8, false);
            clone.setAttr(cloneHolder, "clone_write", std::string("clone-owned"));
            graph.setAttr(holder, "clone_only", std::string("source-overwrite"));
            if (graph.lookupSymbol(cloneOnly).valid() || graph.opAttrRange(holder).find("clone_write") ||
                clone.opAttrRange(cloneHolder).find("clone_only")->value().asString() != cloneOnly)
                return fail("cloned string/symbol entries shared mutable registration or payload state");
        }
        Graph& clone = *cloned.findGraph("payload_first");
        clone.compact();
        const auto holder = clone.findOperation("holder");
        const auto independent = clone.internSymbol("post-source-destruction");
        clone.createValue(independent, 8, false);
        if (!clone.findValue(cloneOnly).valid() || !clone.findValue(promoted).valid() ||
            !clone.findValue(deferred).valid() ||
            clone.opAttrRange(holder).find("clone_only")->value().asString() != cloneOnly ||
            clone.opAttrRange(holder).find("clone_write")->value().asString() != "clone-owned")
            return fail("clone could not independently retain and extend payload-first symbols");
        return 0;
    }

    std::string compareGraphs(const Graph &lhs, const Graph &rhs)
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
            if (lhs.operationSymbol(opId) != rhs.operationSymbol(opId))
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

            auto lhsAttrs = lhs.opAttrRange(opId);
            auto rhsAttrs = rhs.opAttrRange(opId);
            if (lhsAttrs.size() != rhsAttrs.size())
            {
                return "opAttrs size mismatch";
            }
            auto rhsAttr = rhsAttrs.begin();
            for (const auto lhsAttr : lhsAttrs)
            {
                const auto right = *rhsAttr++;
                if (lhsAttr.key() != right.key() || lhsAttr.value().toOwned() != right.value().toOwned())
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
        if (borrowedSourceLocations() != 0) return 1;
        if (copiedSourceLocations() != 0) return 1;
        if (payloadFirstSymbols() != 0) return 1;
        namespace grh_ir = wolvrix::lib::grh;

        grh_ir::Design design;
        grh_ir::Graph& builder = design.createGraph("roundtrip");

        auto symA = builder.internSymbol("a");
        auto symB = builder.internSymbol("b");
        auto symTmp = builder.internSymbol("tmp");
        auto symOutVal = builder.internSymbol("out_val");
        auto symArr = builder.internSymbol("arr");
        auto symArr2 = builder.internSymbol("arr2");
        auto symAdd = builder.internSymbol("add0");
        auto symAssign = builder.internSymbol("assign0");

        auto vA = builder.createValue(symA, 8, false);
        auto vB = builder.createValue(symB, 8, false);
        auto vTmp = builder.createValue(symTmp, 8, true);
        auto vOut = builder.createValue(symOutVal, 8, false);
        const std::vector<int64_t> arrShape{4};
        const std::vector<int64_t> arr2Shape{2, 4};
        auto vArr = builder.createValue(symArr, 32, false, ValueType::Logic, arrShape);
        auto vArr2 = builder.createValue(symArr2, 64, true, ValueType::Logic, arr2Shape);
        (void)vArr;
        (void)vArr2;

        auto opAdd = builder.createOperation(OperationKind::kAdd, symAdd);
        builder.addOperand(opAdd, vA);
        builder.addOperand(opAdd, vB);
        builder.addResult(opAdd, vTmp);

        auto opAssign = builder.createOperation(OperationKind::kAssign, symAssign);
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

        builder.compact();
        grh_ir::Design rebuilt = design.clone();
        grh_ir::Graph& roundTrip = *rebuilt.findGraph("roundtrip");
        roundTrip.compact();

        const std::string mismatch = compareGraphs(builder, roundTrip);
        if (!mismatch.empty())
        {
            return fail(std::string("Graph compact/clone roundtrip ") + mismatch);
        }
    }
    catch (const std::exception &ex)
    {
        return fail(std::string("Unhandled exception: ") + ex.what());
    }
    return 0;
}
