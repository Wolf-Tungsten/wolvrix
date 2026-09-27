#include "core/grh.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "slang/text/Json.h"

namespace wolvrix::lib::grh
{

    bool attributeValueIsJsonSerializable(const AttributeValue &value)
    {
        struct Visitor
        {
            bool operator()(bool) const noexcept { return true; }
            bool operator()(int64_t) const noexcept { return true; }
            bool operator()(double v) const noexcept { return std::isfinite(v); }
            bool operator()(const std::string &) const noexcept { return true; }
            bool operator()(const std::vector<bool> &) const noexcept { return true; }
            bool operator()(const std::vector<int64_t> &) const noexcept { return true; }
            bool operator()(const std::vector<double> &arr) const noexcept
            {
                for (double entry : arr)
                {
                    if (!std::isfinite(entry))
                    {
                        return false;
                    }
                }
                return true;
            }
            bool operator()(const std::vector<std::string> &) const noexcept { return true; }
        };

        return std::visit(Visitor{}, value);
    }

    namespace
    {
        template <typename T>
        std::span<const T> spanForRange(const std::vector<T> &storage, const Range &range)
        {
            if (range.count == 0)
            {
                return {};
            }
            return std::span<const T>(storage.data() + range.offset, range.count);
        }

        std::string symbolTextOrEmpty(const GraphSymbolTable &symbols, SymbolId sym)
        {
            return sym.valid() ? std::string(symbols.text(sym)) : std::string();
        }

        std::string formatSrcLocForDebug(const std::optional<SrcLoc> &srcLoc)
        {
            if (!srcLoc)
            {
                return {};
            }
            if (!srcLoc->file.empty() && srcLoc->line != 0)
            {
                std::string out = srcLoc->file;
                out.push_back(':');
                out += std::to_string(srcLoc->line);
                if (srcLoc->column != 0)
                {
                    out.push_back(':');
                    out += std::to_string(srcLoc->column);
                }
                return out;
            }
            if (!srcLoc->origin.empty() || !srcLoc->pass.empty() || !srcLoc->note.empty())
            {
                std::string out;
                if (!srcLoc->origin.empty())
                {
                    out.append(srcLoc->origin);
                }
                if (!srcLoc->pass.empty())
                {
                    if (!out.empty())
                    {
                        out.push_back(':');
                    }
                    out.append(srcLoc->pass);
                }
                if (!srcLoc->note.empty())
                {
                    if (!out.empty())
                    {
                        out.push_back(':');
                    }
                    out.append(srcLoc->note);
                }
                return out;
            }
            return {};
        }

        ValueId findPortValue(std::span<const Port> ports, std::string_view name) noexcept
        {
            if (name.empty())
            {
                return ValueId::invalid();
            }
            for (const auto &port : ports)
            {
                if (port.name == name)
                {
                    return port.value;
                }
            }
            return ValueId::invalid();
        }

        void cloneGraphContents(const Graph &source, Graph &clone)
        {
            std::unordered_map<uint32_t, SymbolId> symbolMap;
            symbolMap.reserve(source.values().size() + source.operations().size());

            auto mapSymbol = [&](SymbolId sym) -> SymbolId {
                if (!sym.valid())
                {
                    return SymbolId::invalid();
                }
                auto it = symbolMap.find(sym.value);
                if (it != symbolMap.end())
                {
                    return it->second;
                }
                std::string_view text = source.symbolText(sym);
                if (text.empty())
                {
                    throw std::runtime_error("Source symbol text is empty during clone");
                }
                SymbolId cloned = clone.lookupSymbol(text);
                if (!cloned.valid())
                {
                    cloned = clone.internSymbol(text);
                }
                if (!cloned.valid())
                {
                    throw std::runtime_error("Failed to clone symbol: " + std::string(text));
                }
                symbolMap.emplace(sym.value, cloned);
                return cloned;
            };

            std::unordered_map<ValueId, ValueId, ValueIdHash> valueMap;
            valueMap.reserve(source.values().size());
            for (const auto srcValueId : source.values())
            {
                const Value srcValue = source.getValue(srcValueId);
                SymbolId dstSym = mapSymbol(srcValue.symbol());
                ValueId dstValue = clone.createValue(dstSym,
                                                     srcValue.width(),
                                                     srcValue.isSigned(),
                                                     srcValue.type());
                if (srcValue.srcLoc())
                {
                    clone.setValueSrcLoc(dstValue, *srcValue.srcLoc());
                }
                valueMap.emplace(srcValueId, dstValue);
            }

            for (const auto srcOpId : source.operations())
            {
                const Operation srcOp = source.getOperation(srcOpId);
                SymbolId dstSym = mapSymbol(srcOp.symbol());
                OperationId dstOp = clone.createOperation(srcOp.kind(), dstSym);
                for (const auto &attr : srcOp.attrs())
                {
                    clone.setAttr(dstOp, attr.key, attr.value);
                }
                for (const auto srcOperand : srcOp.operands())
                {
                    auto it = valueMap.find(srcOperand);
                    if (it == valueMap.end())
                    {
                        throw std::runtime_error("Missing operand during graph clone");
                    }
                    clone.addOperand(dstOp, it->second);
                }
                for (const auto srcResult : srcOp.results())
                {
                    auto it = valueMap.find(srcResult);
                    if (it == valueMap.end())
                    {
                        throw std::runtime_error("Missing result during graph clone");
                    }
                    clone.addResult(dstOp, it->second);
                }
                if (srcOp.srcLoc())
                {
                    clone.setOpSrcLoc(dstOp, *srcOp.srcLoc());
                }
            }

            for (const auto &port : source.inputPorts())
            {
                auto it = valueMap.find(port.value);
                if (it == valueMap.end())
                {
                    throw std::runtime_error("Missing input port value during graph clone");
                }
                clone.bindInputPort(port.name, it->second);
            }
            for (const auto &port : source.outputPorts())
            {
                auto it = valueMap.find(port.value);
                if (it == valueMap.end())
                {
                    throw std::runtime_error("Missing output port value during graph clone");
                }
                clone.bindOutputPort(port.name, it->second);
            }
            for (const auto &port : source.inoutPorts())
            {
                auto itIn = valueMap.find(port.in);
                auto itOut = valueMap.find(port.out);
                auto itOe = valueMap.find(port.oe);
                if (itIn == valueMap.end() || itOut == valueMap.end() || itOe == valueMap.end())
                {
                    throw std::runtime_error("Missing inout port value during graph clone");
                }
                clone.bindInoutPort(port.name, itIn->second, itOut->second, itOe->second);
            }

            for (const auto srcSym : source.declaredSymbols())
            {
                if (!srcSym.valid())
                {
                    continue;
                }
                std::string_view text = source.symbolText(srcSym);
                if (text.empty())
                {
                    throw std::runtime_error("Declared symbol text is empty during clone");
                }
                SymbolId dstSym = clone.lookupSymbol(text);
                if (!dstSym.valid())
                {
                    dstSym = clone.internSymbol(text);
                }
                if (!dstSym.valid())
                {
                    throw std::runtime_error("Failed to clone declared symbol: " + std::string(text));
                }
                clone.addDeclaredSymbol(dstSym);
            }

            for (const auto &srcGroup : source.generateGroups())
            {
                const SymbolId dstScope = mapSymbol(srcGroup.scope);
                const SymbolId dstName = mapSymbol(srcGroup.name);
                const std::size_t dstGroup = clone.addGenerateGroup(dstScope, dstName);
                for (const auto srcSym : srcGroup.symbols)
                {
                    if (!srcSym.valid())
                    {
                        continue;
                    }
                    clone.addGenerateGroupSymbol(dstGroup, mapSymbol(srcSym));
                }
            }
        }
    } // namespace

    std::size_t SymbolTable::StringHash::operator()(std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t SymbolTable::StringHash::operator()(const std::string &value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }

    bool SymbolTable::StringEq::operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool SymbolTable::StringEq::operator()(const std::string &lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool SymbolTable::StringEq::operator()(const std::string &lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }

    bool SymbolTable::StringEq::operator()(std::string_view lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }

    SymbolTable::SymbolTable()
    {
        textById_.emplace_back();
    }

    DesignSymbolTable::DesignSymbolTable()
    {
        symbolByGraph_.push_back(SymbolId::invalid());
    }

    GraphId DesignSymbolTable::allocateGraphId(SymbolId symbol)
    {
        if (!symbol.valid())
        {
            throw std::runtime_error("Graph symbol is invalid");
        }
        if (!valid(symbol))
        {
            throw std::runtime_error("Graph symbol is not in the symbol table");
        }
        if (graphIndexBySymbol_.contains(symbol.value))
        {
            throw std::runtime_error("Graph symbol already has an assigned GraphId");
        }
        GraphId graphId;
        graphId.index = nextGraphIndex_++;
        graphId.generation = 0;
        if (symbolByGraph_.size() <= graphId.index)
        {
            symbolByGraph_.resize(graphId.index + 1);
        }
        symbolByGraph_[graphId.index] = symbol;
        graphIndexBySymbol_[symbol.value] = graphId.index;
        return graphId;
    }

    void DesignSymbolTable::releaseGraphId(SymbolId symbol)
    {
        if (!symbol.valid())
        {
            return;
        }
        auto it = graphIndexBySymbol_.find(symbol.value);
        if (it == graphIndexBySymbol_.end())
        {
            return;
        }
        const uint32_t graphIndex = it->second;
        graphIndexBySymbol_.erase(it);
        if (graphIndex < symbolByGraph_.size())
        {
            symbolByGraph_[graphIndex] = SymbolId::invalid();
        }
    }

    GraphId DesignSymbolTable::lookupGraphId(SymbolId symbol) const noexcept
    {
        if (!symbol.valid())
        {
            return GraphId::invalid();
        }
        auto it = graphIndexBySymbol_.find(symbol.value);
        if (it == graphIndexBySymbol_.end())
        {
            return GraphId::invalid();
        }
        GraphId graphId;
        graphId.index = it->second;
        graphId.generation = 0;
        return graphId;
    }

    SymbolId DesignSymbolTable::symbolForGraph(GraphId graph) const noexcept
    {
        if (!graph.valid())
        {
            return SymbolId::invalid();
        }
        if (graph.index >= symbolByGraph_.size())
        {
            return SymbolId::invalid();
        }
        return symbolByGraph_[graph.index];
    }

    SymbolId SymbolTable::intern(std::string_view text)
    {
        auto it = symbolsByText_.find(text);
        if (it != symbolsByText_.end())
        {
            return it->second;
        }

        SymbolId id;
        id.value = static_cast<uint32_t>(textById_.size());
        textById_.emplace_back(text);
        symbolsByText_.emplace(textById_.back(), id);
        return id;
    }

    SymbolId SymbolTable::lookup(std::string_view text) const
    {
        auto it = symbolsByText_.find(text);
        if (it == symbolsByText_.end())
        {
            return SymbolId::invalid();
        }
        return it->second;
    }

    bool SymbolTable::contains(std::string_view text) const
    {
        return symbolsByText_.find(text) != symbolsByText_.end();
    }

    std::string_view SymbolTable::text(SymbolId id) const
    {
        if (!valid(id))
        {
            throw std::runtime_error("Invalid SymbolId");
        }
        return textById_[id.value];
    }

    bool SymbolTable::valid(SymbolId id) const noexcept
    {
        return id.value != 0 && id.value < textById_.size();
    }

    void SymbolTable::reserve(std::size_t count)
    {
        symbolsByText_.reserve(count);
    }

    void ValueId::assertGraph(GraphId expected) const
    {
        if (!expected.valid())
        {
            throw std::runtime_error("Expected GraphId is invalid");
        }
        if (!valid())
        {
            throw std::runtime_error("ValueId is invalid");
        }
        if (!graph.valid())
        {
            throw std::runtime_error("ValueId has invalid GraphId");
        }
        if (graph != expected)
        {
            if (std::getenv("WOLF_DEBUG_GRAPH_ID"))
            {
                std::fprintf(stderr,
                             "[graph-id] value index=%u gen=%u graph=%u/%u expected=%u/%u\n",
                             index, generation, graph.index, graph.generation,
                             expected.index, expected.generation);
            }
            throw std::runtime_error("ValueId used with mismatched GraphId");
        }
    }

    void OperationId::assertGraph(GraphId expected) const
    {
        if (!expected.valid())
        {
            throw std::runtime_error("Expected GraphId is invalid");
        }
        if (!valid())
        {
            throw std::runtime_error("OperationId is invalid");
        }
        if (!graph.valid())
        {
            throw std::runtime_error("OperationId has invalid GraphId");
        }
        if (graph != expected)
        {
            if (std::getenv("WOLF_DEBUG_GRAPH_ID"))
            {
                std::fprintf(stderr,
                             "[graph-id] operation index=%u gen=%u graph=%u/%u expected=%u/%u\n",
                             index, generation, graph.index, graph.generation,
                             expected.index, expected.generation);
            }
            throw std::runtime_error("OperationId used with mismatched GraphId");
        }
    }

    std::span<const OperationId> GraphView::operations() const noexcept
    {
        return std::span<const OperationId>(operations_.data(), operations_.size());
    }

    std::span<const ValueId> GraphView::values() const noexcept
    {
        return std::span<const ValueId>(values_.data(), values_.size());
    }

    std::span<const Port> GraphView::inputPorts() const noexcept
    {
        return std::span<const Port>(inputPorts_.data(), inputPorts_.size());
    }

    std::span<const Port> GraphView::outputPorts() const noexcept
    {
        return std::span<const Port>(outputPorts_.data(), outputPorts_.size());
    }

    std::span<const InoutPort> GraphView::inoutPorts() const noexcept
    {
        return std::span<const InoutPort>(inoutPorts_.data(), inoutPorts_.size());
    }

    OperationKind GraphView::opKind(OperationId op) const
    {
        return opKinds_[opIndex(op)];
    }

    std::span<const ValueId> GraphView::opOperands(OperationId op) const
    {
        return spanForRange(operands_, opOperandRanges_[opIndex(op)]);
    }

    std::span<const ValueId> GraphView::opResults(OperationId op) const
    {
        return spanForRange(results_, opResultRanges_[opIndex(op)]);
    }

    SymbolId GraphView::opSymbol(OperationId op) const
    {
        return opSymbols_[opIndex(op)];
    }

    std::span<const AttrKV> GraphView::opAttrs(OperationId op) const
    {
        return spanForRange(opAttrs_, opAttrRanges_[opIndex(op)]);
    }

    std::optional<AttributeValue> GraphView::opAttr(OperationId op, std::string_view key) const
    {
        const auto attrs = opAttrs(op);
        for (const auto &attr : attrs)
        {
            if (attr.key == key)
            {
                return attr.value;
            }
        }
        return std::nullopt;
    }

    std::optional<SrcLoc> GraphView::opSrcLoc(OperationId op) const
    {
        return opSrcLocs_[opIndex(op)];
    }

    SymbolId GraphView::valueSymbol(ValueId value) const
    {
        return valueSymbols_[valueIndex(value)];
    }

    int32_t GraphView::valueWidth(ValueId value) const
    {
        return valueWidths_[valueIndex(value)];
    }

    bool GraphView::valueSigned(ValueId value) const
    {
        return valueSigned_[valueIndex(value)] != 0;
    }

    ValueType GraphView::valueType(ValueId value) const
    {
        if (valueTypes_.empty())
        {
            return ValueType::Logic;
        }
        const std::size_t idx = valueIndex(value);
        if (idx >= valueTypes_.size())
        {
            return ValueType::Logic;
        }
        return static_cast<ValueType>(valueTypes_[idx]);
    }

    bool GraphView::valueIsInput(ValueId value) const
    {
        return valueIsInput_[valueIndex(value)] != 0;
    }

    bool GraphView::valueIsOutput(ValueId value) const
    {
        return valueIsOutput_[valueIndex(value)] != 0;
    }

    bool GraphView::valueIsInout(ValueId value) const
    {
        return valueIsInout_[valueIndex(value)] != 0;
    }

    OperationId GraphView::valueDef(ValueId value) const
    {
        return valueDefs_[valueIndex(value)];
    }

    std::span<const ValueUser> GraphView::valueUsers(ValueId value) const
    {
        return spanForRange(useList_, valueUserRanges_[valueIndex(value)]);
    }

    std::optional<SrcLoc> GraphView::valueSrcLoc(ValueId value) const
    {
        return valueSrcLocs_[valueIndex(value)];
    }

    ValueId GraphView::findValue(SymbolId symbol) const noexcept
    {
        if (!symbol.valid())
        {
            return ValueId::invalid();
        }
        auto it = symbolIndex_.find(symbol.value);
        if (it == symbolIndex_.end() || it->second.kind != SymbolKind::kValue)
        {
            return ValueId::invalid();
        }
        ValueId id;
        id.index = it->second.index;
        id.generation = 0;
        id.graph = graphId_;
        return id;
    }

    OperationId GraphView::findOperation(SymbolId symbol) const noexcept
    {
        if (!symbol.valid())
        {
            return OperationId::invalid();
        }
        auto it = symbolIndex_.find(symbol.value);
        if (it == symbolIndex_.end() || it->second.kind != SymbolKind::kOperation)
        {
            return OperationId::invalid();
        }
        OperationId id;
        id.index = it->second.index;
        id.generation = 0;
        id.graph = graphId_;
        return id;
    }

    std::size_t GraphView::opIndex(OperationId op) const
    {
        op.assertGraph(graphId_);
        const std::size_t index = static_cast<std::size_t>(op.index);
        if (index == 0 || index > opKinds_.size())
        {
            throw std::runtime_error("OperationId out of range");
        }
        return index - 1;
    }

    std::size_t GraphView::valueIndex(ValueId value) const
    {
        value.assertGraph(graphId_);
        const std::size_t index = static_cast<std::size_t>(value.index);
        if (index == 0 || index > valueWidths_.size())
        {
            throw std::runtime_error("ValueId out of range");
        }
        return index - 1;
    }

    GraphBuilder::GraphBuilder(GraphId graphId) : graphId_(graphId)
    {
        if (!graphId_.valid())
        {
            throw std::runtime_error("GraphBuilder requires a valid GraphId");
        }
    }

    GraphBuilder::GraphBuilder(GraphSymbolTable &symbols, GraphId graphId) : GraphBuilder(graphId)
    {
        symbols_ = &symbols;
    }

    void GraphBuilder::reserveValues(std::size_t count)
    {
        values_.reserve(count);
        valueUsers_.reserve(count);
    }

    void GraphBuilder::reserveOperations(std::size_t count)
    {
        operations_.reserve(count);
    }

    void GraphBuilder::reserveSymbols(std::size_t count)
    {
        symbolIndex_.reserve(count);
    }

    void GraphBuilder::reserveOpOperands(OperationId op, std::size_t count)
    {
        operations_[opIndex(op)].operands.reserve(count);
    }

    void GraphBuilder::reserveOpResults(OperationId op, std::size_t count)
    {
        operations_[opIndex(op)].results.reserve(count);
    }

    void GraphBuilder::reserveOpAttrs(OperationId op, std::size_t count)
    {
        operations_[opIndex(op)].attrs.reserve(count);
    }

    GraphBuilder GraphBuilder::fromView(const GraphView &view, GraphSymbolTable &symbols)
    {
        if (!view.graphId_.valid())
        {
            throw std::runtime_error("GraphView has invalid GraphId");
        }

        GraphBuilder builder(symbols, view.graphId_);

        auto require = [](bool condition, const char *message) {
            if (!condition)
            {
                throw std::runtime_error(message);
            }
        };

        const std::size_t valueCount = view.values_.size();
        const std::size_t opCount = view.operations_.size();

        require(view.valueSymbols_.size() == valueCount, "GraphView value metadata size mismatch");
        require(view.valueWidths_.size() == valueCount, "GraphView value metadata size mismatch");
        require(view.valueSigned_.size() == valueCount, "GraphView value metadata size mismatch");
        if (!view.valueTypes_.empty())
        {
            require(view.valueTypes_.size() == valueCount, "GraphView value metadata size mismatch");
        }
        require(view.valueIsInput_.size() == valueCount, "GraphView value metadata size mismatch");
        require(view.valueIsOutput_.size() == valueCount, "GraphView value metadata size mismatch");
        require(view.valueIsInout_.size() == valueCount, "GraphView value metadata size mismatch");
        require(view.valueDefs_.size() == valueCount, "GraphView value metadata size mismatch");
        require(view.valueUserRanges_.size() == valueCount, "GraphView value user range size mismatch");
        require(view.valueSrcLocs_.size() == valueCount, "GraphView value metadata size mismatch");

        require(view.opKinds_.size() == opCount, "GraphView operation metadata size mismatch");
        require(view.opSymbols_.size() == opCount, "GraphView operation metadata size mismatch");
        require(view.opOperandRanges_.size() == opCount, "GraphView operation metadata size mismatch");
        require(view.opResultRanges_.size() == opCount, "GraphView operation metadata size mismatch");
        require(view.opAttrRanges_.size() == opCount, "GraphView operation metadata size mismatch");
        require(view.opSrcLocs_.size() == opCount, "GraphView operation metadata size mismatch");

        auto checkRangeBounds = [&](const Range &range, std::size_t total, const char *label) {
            if (range.offset > total || range.offset + range.count > total)
            {
                throw std::runtime_error(std::string("GraphView ") + label + " range out of bounds");
            }
        };

        std::size_t operandOffset = 0;
        for (std::size_t i = 0; i < opCount; ++i)
        {
            const Range &range = view.opOperandRanges_[i];
            require(range.offset == operandOffset, "GraphView operand ranges are not contiguous");
            checkRangeBounds(range, view.operands_.size(), "operand");
            operandOffset = range.offset + range.count;
        }
        require(operandOffset == view.operands_.size(), "GraphView operand range size mismatch");

        std::size_t resultOffset = 0;
        for (std::size_t i = 0; i < opCount; ++i)
        {
            const Range &range = view.opResultRanges_[i];
            require(range.offset == resultOffset, "GraphView result ranges are not contiguous");
            checkRangeBounds(range, view.results_.size(), "result");
            resultOffset = range.offset + range.count;
        }
        require(resultOffset == view.results_.size(), "GraphView result range size mismatch");

        std::size_t attrOffset = 0;
        for (std::size_t i = 0; i < opCount; ++i)
        {
            const Range &range = view.opAttrRanges_[i];
            require(range.offset == attrOffset, "GraphView attribute ranges are not contiguous");
            checkRangeBounds(range, view.opAttrs_.size(), "attribute");
            attrOffset = range.offset + range.count;
        }
        require(attrOffset == view.opAttrs_.size(), "GraphView attribute range size mismatch");

        std::size_t userOffset = 0;
        for (std::size_t i = 0; i < valueCount; ++i)
        {
            const Range &range = view.valueUserRanges_[i];
            require(range.offset == userOffset, "GraphView use-list ranges are not contiguous");
            checkRangeBounds(range, view.useList_.size(), "use-list");
            userOffset = range.offset + range.count;
        }
        require(userOffset == view.useList_.size(), "GraphView use-list size mismatch");

        builder.values_.reserve(valueCount);
        builder.valueUsers_.reserve(valueCount);
        for (std::size_t i = 0; i < valueCount; ++i)
        {
            const ValueId valueId = view.values_[i];
            valueId.assertGraph(view.graphId_);
            require(valueId.generation == 0, "GraphView value generation must be zero");
            require(valueId.index == i + 1, "GraphView values must be contiguous");

            ValueData data;
            data.symbol = view.valueSymbols_[i];
            builder.validateSymbol(data.symbol, "Value");
            const int32_t width = view.valueWidths_[i];
            ValueType type = ValueType::Logic;
            if (!view.valueTypes_.empty())
            {
                type = static_cast<ValueType>(view.valueTypes_[i]);
            }
            if (type == ValueType::Logic && width <= 0)
            {
                throw std::runtime_error("GraphView value width must be positive");
            }
            data.width = width > 0 ? width : 1;
            data.isSigned = view.valueSigned_[i] != 0;
            data.type = type;
            data.isInput = false;
            data.isOutput = false;
            data.isInout = false;
            data.definingOp = OperationId::invalid();
            data.srcLoc = view.valueSrcLocs_[i];
            data.alive = true;
            builder.values_.push_back(std::move(data));
            builder.bindSymbol(builder.values_.back().symbol,
                               GraphBuilder::SymbolKind::kValue,
                               static_cast<uint32_t>(i + 1),
                               "Value");
        }

        builder.operations_.reserve(opCount);
        for (std::size_t i = 0; i < opCount; ++i)
        {
            const OperationId opId = view.operations_[i];
            opId.assertGraph(view.graphId_);
            require(opId.generation == 0, "GraphView operation generation must be zero");
            require(opId.index == i + 1, "GraphView operations must be contiguous");

            OperationData opData;
            opData.kind = view.opKinds_[i];
            opData.symbol = view.opSymbols_[i];
            builder.validateSymbol(opData.symbol, "Operation");
            opData.srcLoc = view.opSrcLocs_[i];
            opData.alive = true;
            builder.bindSymbol(opData.symbol,
                               GraphBuilder::SymbolKind::kOperation,
                               static_cast<uint32_t>(i + 1),
                               "Operation");

            const Range &operandRange = view.opOperandRanges_[i];
            opData.operands.reserve(operandRange.count);
            for (std::size_t j = 0; j < operandRange.count; ++j)
            {
                const ValueId operand = view.operands_[operandRange.offset + j];
                operand.assertGraph(view.graphId_);
                require(operand.generation == 0, "GraphView operand generation must be zero");
                if (operand.index == 0 || operand.index > valueCount)
                {
                    throw std::runtime_error("GraphView operand refers to invalid value");
                }
                opData.operands.push_back(operand);
            }

            const Range &resultRange = view.opResultRanges_[i];
            opData.results.reserve(resultRange.count);
            for (std::size_t j = 0; j < resultRange.count; ++j)
            {
                const ValueId result = view.results_[resultRange.offset + j];
                result.assertGraph(view.graphId_);
                require(result.generation == 0, "GraphView result generation must be zero");
                if (result.index == 0 || result.index > valueCount)
                {
                    throw std::runtime_error("GraphView result refers to invalid value");
                }
                const std::size_t valIdx = static_cast<std::size_t>(result.index - 1);
                if (builder.values_[valIdx].definingOp.valid())
                {
                    throw std::runtime_error("GraphView value has multiple defining operations");
                }
                builder.values_[valIdx].definingOp = opId;
                opData.results.push_back(result);
            }

            const Range &attrRange = view.opAttrRanges_[i];
            opData.attrs.reserve(attrRange.count);
            for (std::size_t j = 0; j < attrRange.count; ++j)
            {
                const AttrKV &attr = view.opAttrs_[attrRange.offset + j];
                if (!attributeValueIsJsonSerializable(attr.value))
                {
                    throw std::runtime_error("GraphView attribute value must be JSON-serializable");
                }
                opData.attrs.push_back(attr);
            }

            builder.operations_.push_back(std::move(opData));
        }

        for (std::size_t i = 0; i < valueCount; ++i)
        {
            OperationId viewDef = view.valueDefs_[i];
            if (viewDef.valid())
            {
                viewDef.assertGraph(view.graphId_);
                require(viewDef.generation == 0, "GraphView value definition generation must be zero");
                if (viewDef.index == 0 || viewDef.index > opCount)
                {
                    throw std::runtime_error("GraphView value definition refers to invalid operation");
                }
            }
            if (builder.values_[i].definingOp != viewDef)
            {
                throw std::runtime_error("GraphView value definition mismatch");
            }
        }

        builder.inputPorts_ = view.inputPorts_;
        builder.outputPorts_ = view.outputPorts_;
        builder.inoutPorts_ = view.inoutPorts_;

        for (const auto &port : builder.inputPorts_)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Input port name is empty");
            }
            port.value.assertGraph(view.graphId_);
            require(port.value.generation == 0, "GraphView input port value generation must be zero");
            if (port.value.index == 0 || port.value.index > valueCount)
            {
                throw std::runtime_error("GraphView input port refers to invalid value");
            }
        }
        for (const auto &port : builder.outputPorts_)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Output port name is empty");
            }
            port.value.assertGraph(view.graphId_);
            require(port.value.generation == 0, "GraphView output port value generation must be zero");
            if (port.value.index == 0 || port.value.index > valueCount)
            {
                throw std::runtime_error("GraphView output port refers to invalid value");
            }
        }
        for (const auto &port : builder.inoutPorts_)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Inout port name is empty");
            }
            port.in.assertGraph(view.graphId_);
            port.out.assertGraph(view.graphId_);
            port.oe.assertGraph(view.graphId_);
            require(port.in.generation == 0, "GraphView inout port input generation must be zero");
            require(port.out.generation == 0, "GraphView inout port output generation must be zero");
            require(port.oe.generation == 0, "GraphView inout port enable generation must be zero");
            if (port.in.index == 0 || port.in.index > valueCount ||
                port.out.index == 0 || port.out.index > valueCount ||
                port.oe.index == 0 || port.oe.index > valueCount)
            {
                throw std::runtime_error("GraphView inout port refers to invalid value");
            }
        }

        builder.recomputePortFlags();

        for (std::size_t i = 0; i < valueCount; ++i)
        {
            const bool viewInput = view.valueIsInput_[i] != 0;
            if (builder.values_[i].isInput != viewInput)
            {
                throw std::runtime_error("GraphView input port flag mismatch");
            }
            const bool viewOutput = view.valueIsOutput_[i] != 0;
            if (builder.values_[i].isOutput != viewOutput)
            {
                throw std::runtime_error("GraphView output port flag mismatch");
            }
            const bool viewInout = view.valueIsInout_[i] != 0;
            if (builder.values_[i].isInout != viewInout)
            {
                throw std::runtime_error("GraphView inout port flag mismatch");
            }
        }

        std::vector<std::vector<ValueUser>> expectedUsers(valueCount);
        for (std::size_t i = 0; i < opCount; ++i)
        {
            const OperationId opId = view.operations_[i];
            const auto &operands = builder.operations_[i].operands;
            for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
            {
                const ValueId valueId = operands[operandIndex];
                expectedUsers[valueId.index - 1].push_back(
                    ValueUser{opId, static_cast<uint32_t>(operandIndex)});
            }
        }

        for (std::size_t i = 0; i < valueCount; ++i)
        {
            const ValueId valueId = view.values_[i];
            auto actualUsers = view.valueUsers(valueId);
            const auto &expected = expectedUsers[i];
            if (actualUsers.size() != expected.size())
            {
                throw std::runtime_error("GraphView use-list mismatch");
            }
            for (std::size_t j = 0; j < expected.size(); ++j)
            {
                if (actualUsers[j].operation != expected[j].operation ||
                    actualUsers[j].operandIndex != expected[j].operandIndex)
                {
                    throw std::runtime_error("GraphView use-list mismatch");
                }
            }
        }

        builder.valueUsers_ = std::move(expectedUsers);

        return builder;
    }

    ValueId GraphBuilder::addValue(SymbolId sym, int32_t width, bool isSigned, ValueType type)
    {
        if (type == ValueType::Logic && width <= 0)
        {
            throw std::runtime_error("Value width must be positive");
        }
        if (type != ValueType::Logic && width <= 0)
        {
            width = 1;
        }
        validateSymbol(sym, "Value");

        ValueId id;
        id.index = static_cast<uint32_t>(values_.size() + 1);
        id.generation = 0;
        id.graph = graphId_;
        bindSymbol(sym, SymbolKind::kValue, id.index, "Value");
        ValueData data;
        data.symbol = sym;
        data.width = width;
        data.isSigned = isSigned;
        data.type = type;
        values_.push_back(std::move(data));
        valueUsers_.emplace_back();
        return id;
    }

    OperationId GraphBuilder::addOp(OperationKind kind, SymbolId sym)
    {
        validateSymbol(sym, "Operation");

        OperationId id;
        id.index = static_cast<uint32_t>(operations_.size() + 1);
        id.generation = 0;
        id.graph = graphId_;
        bindSymbol(sym, SymbolKind::kOperation, id.index, "Operation");
        OperationData data;
        data.kind = kind;
        data.symbol = sym;
        operations_.push_back(std::move(data));
        return id;
    }

    void GraphBuilder::addOperand(OperationId op, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &operands = operations_[opIdx].operands;
        operands.push_back(value);
        addValueUser(value, op, operands.size() - 1);
    }

    void GraphBuilder::insertOperand(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &operands = operations_[opIdx].operands;
        if (index > operands.size())
        {
            throw std::runtime_error("Operand index out of range");
        }
        const std::vector<ValueId> oldOperands = operands;
        operands.insert(operands.begin() + static_cast<std::ptrdiff_t>(index), value);
        removeOpUses(op, oldOperands);
        addOpUses(op, operands);
    }

    void GraphBuilder::addResult(OperationId op, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        ValueData &data = values_[valIdx];
        if (data.definingOp.valid())
        {
            auto describeValue = [&](ValueId id) -> std::string {
                if (!id.valid())
                {
                    return "value=<invalid>";
                }
                std::string out = "value_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > values_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const ValueData &valueData = values_[idx - 1];
                if (symbols_ && valueData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(valueData.symbol)) + ")";
                }
                out += " w=" + std::to_string(valueData.width);
                out += valueData.isSigned ? " signed" : " unsigned";
                if (valueData.isInput)
                {
                    out += " input";
                }
                if (valueData.isOutput)
                {
                    out += " output";
                }
                const std::string loc = formatSrcLocForDebug(valueData.srcLoc);
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };
            auto describeOp = [&](OperationId id) -> std::string {
                if (!id.valid())
                {
                    return "op=<invalid>";
                }
                std::string out = "op_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > operations_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const OperationData &opData = operations_[idx - 1];
                out += " kind=";
                out += std::string(toString(opData.kind));
                if (symbols_ && opData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(opData.symbol)) + ")";
                }
                const std::string loc = formatSrcLocForDebug(opData.srcLoc);
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };

            std::string message = "Value already has a defining operation; ";
            message += describeValue(value);
            message += "; new_def=";
            message += describeOp(op);
            message += "; existing_def=";
            message += describeOp(data.definingOp);
            throw std::runtime_error(message);
        }
        data.definingOp = op;
        if (!data.srcLoc && operations_[opIdx].srcLoc)
        {
            data.srcLoc = operations_[opIdx].srcLoc;
        }
        operations_[opIdx].results.push_back(value);
    }

    void GraphBuilder::insertResult(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &results = operations_[opIdx].results;
        if (index > results.size())
        {
            throw std::runtime_error("Result index out of range");
        }
        ValueData &data = values_[valIdx];
        if (data.definingOp.valid())
        {
            auto describeValue = [&](ValueId id) -> std::string {
                if (!id.valid())
                {
                    return "value=<invalid>";
                }
                std::string out = "value_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > values_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const ValueData &valueData = values_[idx - 1];
                if (symbols_ && valueData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(valueData.symbol)) + ")";
                }
                out += " w=" + std::to_string(valueData.width);
                out += valueData.isSigned ? " signed" : " unsigned";
                if (valueData.isInput)
                {
                    out += " input";
                }
                if (valueData.isOutput)
                {
                    out += " output";
                }
                const std::string loc = formatSrcLocForDebug(valueData.srcLoc);
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };
            auto describeOp = [&](OperationId id) -> std::string {
                if (!id.valid())
                {
                    return "op=<invalid>";
                }
                std::string out = "op_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > operations_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const OperationData &opData = operations_[idx - 1];
                out += " kind=";
                out += std::string(toString(opData.kind));
                if (symbols_ && opData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(opData.symbol)) + ")";
                }
                const std::string loc = formatSrcLocForDebug(opData.srcLoc);
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };

            std::string message = "Value already has a defining operation; ";
            message += describeValue(value);
            message += "; new_def=";
            message += describeOp(op);
            message += "; existing_def=";
            message += describeOp(data.definingOp);
            throw std::runtime_error(message);
        }
        data.definingOp = op;
        if (!data.srcLoc && operations_[opIdx].srcLoc)
        {
            data.srcLoc = operations_[opIdx].srcLoc;
        }
        results.insert(results.begin() + static_cast<std::ptrdiff_t>(index), value);
    }

    void GraphBuilder::replaceOperand(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        const std::size_t valIdx = valueIndex(value);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        auto &operands = operations_[opIdx].operands;
        if (index >= operands.size())
        {
            throw std::runtime_error("Operand index out of range");
        }
        const ValueId current = operands[index];
        if (current == value)
        {
            return;
        }
        operands[index] = value;
        removeValueUser(current, op, index);
        addValueUser(value, op, index);
    }

    void GraphBuilder::replaceResult(OperationId op, std::size_t index, ValueId value)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }

        auto &results = operations_[opIdx].results;
        if (index >= results.size())
        {
            throw std::runtime_error("Result index out of range");
        }

        const ValueId current = results[index];
        if (current == value)
        {
            return;
        }
        if (values_[valIdx].definingOp.valid())
        {
            auto describeValue = [&](ValueId id) -> std::string {
                if (!id.valid())
                {
                    return "value=<invalid>";
                }
                std::string out = "value_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > values_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const ValueData &valueData = values_[idx - 1];
                if (symbols_ && valueData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(valueData.symbol)) + ")";
                }
                out += " w=" + std::to_string(valueData.width);
                out += valueData.isSigned ? " signed" : " unsigned";
                if (valueData.isInput)
                {
                    out += " input";
                }
                if (valueData.isOutput)
                {
                    out += " output";
                }
                const std::string loc = formatSrcLocForDebug(valueData.srcLoc);
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };
            auto describeOp = [&](OperationId id) -> std::string {
                if (!id.valid())
                {
                    return "op=<invalid>";
                }
                std::string out = "op_" + std::to_string(id.index);
                if (id.graph != graphId_)
                {
                    out += " graph_mismatch";
                    return out;
                }
                const std::size_t idx = static_cast<std::size_t>(id.index);
                if (idx == 0 || idx > operations_.size())
                {
                    out += " out_of_range";
                    return out;
                }
                const OperationData &opData = operations_[idx - 1];
                out += " kind=";
                out += std::string(toString(opData.kind));
                if (symbols_ && opData.symbol.valid())
                {
                    out += " (" + std::string(symbols_->text(opData.symbol)) + ")";
                }
                const std::string loc = formatSrcLocForDebug(opData.srcLoc);
                if (!loc.empty())
                {
                    out += " @";
                    out += loc;
                }
                return out;
            };

            std::string message = "Value already has a defining operation while replacing result ";
            message += std::to_string(index);
            message += "; ";
            message += describeValue(value);
            message += "; new_def=";
            message += describeOp(op);
            message += "; existing_def=";
            message += describeOp(values_[valIdx].definingOp);
            throw std::runtime_error(message);
        }

        const std::size_t currentIdx = valueIndex(current);
        if (values_[currentIdx].definingOp == op)
        {
            values_[currentIdx].definingOp = OperationId::invalid();
        }
        values_[valIdx].definingOp = op;
        results[index] = value;
    }

    void GraphBuilder::replaceAllUses(ValueId from, ValueId to)
    {
        replaceAllUsesInternal(from, to, std::nullopt);
    }

    bool GraphBuilder::eraseOperand(OperationId op, std::size_t index)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        auto &operands = operations_[opIdx].operands;
        if (index >= operands.size())
        {
            return false;
        }
        const std::vector<ValueId> oldOperands = operands;
        operands.erase(operands.begin() + static_cast<std::ptrdiff_t>(index));
        removeOpUses(op, oldOperands);
        addOpUses(op, operands);
        return true;
    }

    bool GraphBuilder::eraseResult(OperationId op, std::size_t index)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        auto &results = operations_[opIdx].results;
        if (index >= results.size())
        {
            return false;
        }
        const ValueId value = results[index];
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            return false;
        }
        if (countValueUses(value, std::nullopt) != 0)
        {
            return false;
        }
        if (values_[valIdx].isInput || values_[valIdx].isOutput)
        {
            return false;
        }
        for (const auto &port : inoutPorts_)
        {
            if (port.in == value || port.out == value || port.oe == value)
            {
                return false;
            }
        }
        values_[valIdx].definingOp = OperationId::invalid();
        results.erase(results.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    bool GraphBuilder::eraseOp(OperationId op)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        const auto &results = operations_[opIdx].results;
        for (const auto &result : results)
        {
            if (countValueUses(result, op) != 0)
            {
                return false;
            }
        }
        for (const auto &result : results)
        {
            const std::size_t valIdx = valueIndex(result);
            if (values_[valIdx].definingOp == op)
            {
                values_[valIdx].definingOp = OperationId::invalid();
            }
        }
        removeOpUses(op, operations_[opIdx].operands);
        const SymbolId symbol = operations_[opIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        operations_[opIdx].alive = false;
        return true;
    }

    bool GraphBuilder::eraseOpUnchecked(OperationId op)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        const auto &results = operations_[opIdx].results;
        for (const auto &result : results)
        {
            const std::size_t valIdx = valueIndex(result);
            if (values_[valIdx].definingOp == op)
            {
                values_[valIdx].definingOp = OperationId::invalid();
            }
        }
        removeOpUses(op, operations_[opIdx].operands);
        const SymbolId symbol = operations_[opIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        operations_[opIdx].alive = false;
        return true;
    }

    bool GraphBuilder::eraseOp(OperationId op, std::span<const ValueId> replacementResults)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        const auto &results = operations_[opIdx].results;
        if (replacementResults.size() != results.size())
        {
            return false;
        }
        for (const auto &value : replacementResults)
        {
            const std::size_t valIdx = valueIndex(value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("Replacement value is erased");
            }
        }
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            const ValueId from = results[i];
            const ValueId to = replacementResults[i];
            if (from == to)
            {
                continue;
            }
            replaceAllUsesInternal(from, to, op);
        }
        for (const auto &result : results)
        {
            const std::size_t valIdx = valueIndex(result);
            if (values_[valIdx].definingOp == op)
            {
                values_[valIdx].definingOp = OperationId::invalid();
            }
        }
        removeOpUses(op, operations_[opIdx].operands);
        const SymbolId symbol = operations_[opIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        operations_[opIdx].alive = false;
        return true;
    }

    bool GraphBuilder::eraseValue(ValueId value)
    {
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            return false;
        }
        if (countValueUses(value, std::nullopt) != 0)
        {
            return false;
        }
        if (values_[valIdx].isInput || values_[valIdx].isOutput)
        {
            return false;
        }
        for (const auto &port : inoutPorts_)
        {
            if (port.in == value || port.out == value || port.oe == value)
            {
                return false;
            }
        }
        if (values_[valIdx].definingOp.valid())
        {
            return false;
        }
        const SymbolId symbol = values_[valIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1));
        }
        valueUsers_[valIdx].clear();
        values_[valIdx].alive = false;
        return true;
    }

    bool GraphBuilder::eraseValueUnchecked(ValueId value)
    {
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            return false;
        }
        if (values_[valIdx].isInput || values_[valIdx].isOutput)
        {
            return false;
        }
        for (const auto &port : inoutPorts_)
        {
            if (port.in == value || port.out == value || port.oe == value)
            {
                return false;
            }
        }
        if (values_[valIdx].definingOp.valid())
        {
            return false;
        }
        const SymbolId symbol = values_[valIdx].symbol;
        if (symbol.valid())
        {
            unbindSymbol(symbol, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1));
        }
        values_[valIdx].alive = false;
        return true;
    }

    void GraphBuilder::bindPort(std::vector<Port> &ports, std::string_view name, ValueId value, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        bool updated = false;
        for (auto &port : ports)
        {
            if (port.name == name)
            {
                port.value = value;
                updated = true;
                break;
            }
        }
        if (!updated)
        {
            ports.push_back(Port{std::string(name), value});
        }
        recomputePortFlags();
    }

    void GraphBuilder::bindInputPorts(std::span<const Port> ports)
    {
        for (const auto &port : ports)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Input port name is empty");
            }
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            bool updated = false;
            for (auto &entry : inputPorts_)
            {
                if (entry.name == port.name)
                {
                    entry.value = port.value;
                    updated = true;
                    break;
                }
            }
            if (!updated)
            {
                inputPorts_.push_back(Port{port.name, port.value});
            }
        }
        recomputePortFlags();
    }

    void GraphBuilder::bindOutputPorts(std::span<const Port> ports)
    {
        for (const auto &port : ports)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Output port name is empty");
            }
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            bool updated = false;
            for (auto &entry : outputPorts_)
            {
                if (entry.name == port.name)
                {
                    entry.value = port.value;
                    updated = true;
                    break;
                }
            }
            if (!updated)
            {
                outputPorts_.push_back(Port{port.name, port.value});
            }
        }
        recomputePortFlags();
    }

    void GraphBuilder::bindInoutPorts(std::span<const InoutPort> ports)
    {
        for (const auto &port : ports)
        {
            if (port.name.empty())
            {
                throw std::runtime_error("Inout port name is empty");
            }
            const std::size_t inIdx = valueIndex(port.in);
            const std::size_t outIdx = valueIndex(port.out);
            const std::size_t oeIdx = valueIndex(port.oe);
            if (!values_[inIdx].alive || !values_[outIdx].alive || !values_[oeIdx].alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            bool updated = false;
            for (auto &entry : inoutPorts_)
            {
                if (entry.name == port.name)
                {
                    entry.in = port.in;
                    entry.out = port.out;
                    entry.oe = port.oe;
                    updated = true;
                    break;
                }
            }
            if (!updated)
            {
                inoutPorts_.push_back(InoutPort{port.name, port.in, port.out, port.oe});
            }
        }
        recomputePortFlags();
    }

    void GraphBuilder::bindInoutPort(std::vector<InoutPort> &ports, std::string_view name, ValueId in,
                                     ValueId out, ValueId oe, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        const std::size_t inIdx = valueIndex(in);
        const std::size_t outIdx = valueIndex(out);
        const std::size_t oeIdx = valueIndex(oe);
        if (!values_[inIdx].alive || !values_[outIdx].alive || !values_[oeIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        bool updated = false;
        for (auto &port : ports)
        {
            if (port.name == name)
            {
                port.in = in;
                port.out = out;
                port.oe = oe;
                updated = true;
                break;
            }
        }
        if (!updated)
        {
            ports.push_back(InoutPort{std::string(name), in, out, oe});
        }
        recomputePortFlags();
    }

    bool GraphBuilder::removePort(std::vector<Port> &ports, std::string_view name, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        for (auto it = ports.begin(); it != ports.end(); ++it)
        {
            if (it->name == name)
            {
                ports.erase(it);
                recomputePortFlags();
                return true;
            }
        }
        return false;
    }

    bool GraphBuilder::removeInoutPort(std::vector<InoutPort> &ports, std::string_view name, std::string_view context)
    {
        if (name.empty())
        {
            throw std::runtime_error(std::string(context) + " name is empty");
        }
        for (auto it = ports.begin(); it != ports.end(); ++it)
        {
            if (it->name == name)
            {
                ports.erase(it);
                recomputePortFlags();
                return true;
            }
        }
        return false;
    }

    void GraphBuilder::bindInputPort(std::string_view name, ValueId value)
    {
        bindPort(inputPorts_, name, value, "Input port");
    }

    void GraphBuilder::bindOutputPort(std::string_view name, ValueId value)
    {
        bindPort(outputPorts_, name, value, "Output port");
    }

    void GraphBuilder::bindInoutPort(std::string_view name, ValueId in, ValueId out, ValueId oe)
    {
        bindInoutPort(inoutPorts_, name, in, out, oe, "Inout port");
    }

    bool GraphBuilder::removeInputPort(std::string_view name)
    {
        return removePort(inputPorts_, name, "Input port");
    }

    bool GraphBuilder::removeOutputPort(std::string_view name)
    {
        return removePort(outputPorts_, name, "Output port");
    }

    bool GraphBuilder::removeInoutPort(std::string_view name)
    {
        return removeInoutPort(inoutPorts_, name, "Inout port");
    }

    void GraphBuilder::setAttr(OperationId op, std::string_view key, AttributeValue value)
    {
        if (!attributeValueIsJsonSerializable(value))
        {
            throw std::runtime_error("Attribute value must be JSON-serializable");
        }
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        auto &attrs = operations_[opIdx].attrs;
        auto it = std::lower_bound(attrs.begin(), attrs.end(), key,
                                   [](const AttrKV &attr, std::string_view attrKey) {
                                       return attr.key < attrKey;
                                   });
        if (it != attrs.end() && it->key == key)
        {
            it->value = std::move(value);
            return;
        }
        attrs.insert(it, AttrKV{std::string(key), std::move(value)});
    }

    void GraphBuilder::setOpKind(OperationId op, OperationKind kind)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        operations_[opIdx].kind = kind;
    }

    bool GraphBuilder::eraseAttr(OperationId op, std::string_view key)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            return false;
        }
        auto &attrs = operations_[opIdx].attrs;
        for (auto it = attrs.begin(); it != attrs.end(); ++it)
        {
            if (it->key == key)
            {
                attrs.erase(it);
                return true;
            }
        }
        return false;
    }

    void GraphBuilder::setValueSrcLoc(ValueId value, SrcLoc loc)
    {
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        values_[valIdx].srcLoc = std::move(loc);
    }

    void GraphBuilder::setOpSrcLoc(OperationId op, SrcLoc loc)
    {
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        operations_[opIdx].srcLoc = std::move(loc);
    }

    void GraphBuilder::setOpSymbol(OperationId op, SymbolId sym)
    {
        validateSymbol(sym, "Operation");
        const std::size_t opIdx = opIndex(op);
        if (!operations_[opIdx].alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }
        const SymbolId old = operations_[opIdx].symbol;
        if (old == sym)
        {
            return;
        }
        if (symbolIndex_.find(sym.value) != symbolIndex_.end())
        {
            throw std::runtime_error("Operation symbol already bound to value or operation");
        }
        if (old.valid())
        {
            unbindSymbol(old, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1));
        }
        bindSymbol(sym, SymbolKind::kOperation, static_cast<uint32_t>(opIdx + 1), "Operation");
        operations_[opIdx].symbol = sym;
    }

    void GraphBuilder::setValueSymbol(ValueId value, SymbolId sym)
    {
        validateSymbol(sym, "Value");
        const std::size_t valIdx = valueIndex(value);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        const SymbolId old = values_[valIdx].symbol;
        if (old == sym)
        {
            return;
        }
        if (symbolIndex_.find(sym.value) != symbolIndex_.end())
        {
            throw std::runtime_error("Value symbol already bound to value or operation");
        }
        if (old.valid())
        {
            unbindSymbol(old, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1));
        }
        bindSymbol(sym, SymbolKind::kValue, static_cast<uint32_t>(valIdx + 1), "Value");
        values_[valIdx].symbol = sym;
    }

    void GraphBuilder::clearOpSymbol(OperationId op)
    {
        throw std::runtime_error("Clearing operation symbol is not allowed");
    }

    void GraphBuilder::clearValueSymbol(ValueId value)
    {
        throw std::runtime_error("Clearing value symbol is not allowed");
    }

    GraphView GraphBuilder::freeze() const
    {
        GraphView view;
        view.graphId_ = graphId_;

        std::vector<uint32_t> valueRemap(values_.size() + 1, 0);
        std::vector<uint32_t> opRemap(operations_.size() + 1, 0);
        std::size_t valueCount = 0;
        std::size_t opCount = 0;
        for (std::size_t i = 0; i < values_.size(); ++i)
        {
            if (!values_[i].alive)
            {
                continue;
            }
            valueRemap[i + 1] = static_cast<uint32_t>(++valueCount);
        }
        for (std::size_t i = 0; i < operations_.size(); ++i)
        {
            if (!operations_[i].alive)
            {
                continue;
            }
            opRemap[i + 1] = static_cast<uint32_t>(++opCount);
        }

        auto remapValue = [&](ValueId value) -> ValueId {
            value.assertGraph(graphId_);
            if (value.index == 0 || value.index >= valueRemap.size())
            {
                throw std::runtime_error("ValueId out of range during freeze");
            }
            uint32_t newIndex = valueRemap[value.index];
            if (newIndex == 0)
            {
                throw std::runtime_error("ValueId refers to erased value during freeze");
            }
            ValueId remapped;
            remapped.index = newIndex;
            remapped.generation = 0;
            remapped.graph = graphId_;
            return remapped;
        };

        auto remapOp = [&](OperationId op) -> OperationId {
            op.assertGraph(graphId_);
            if (op.index == 0 || op.index >= opRemap.size())
            {
                throw std::runtime_error("OperationId out of range during freeze");
            }
            uint32_t newIndex = opRemap[op.index];
            if (newIndex == 0)
            {
                return OperationId::invalid();
            }
            OperationId remapped;
            remapped.index = newIndex;
            remapped.generation = 0;
            remapped.graph = graphId_;
            return remapped;
        };

        auto portLess = [&](const Port &lhs, const Port &rhs) {
            return lhs.name < rhs.name;
        };
        auto inoutPortLess = [&](const InoutPort &lhs, const InoutPort &rhs) {
            return lhs.name < rhs.name;
        };

        auto bindViewSymbol = [&](SymbolId sym, GraphView::SymbolKind kind, uint32_t index, std::string_view context) {
            if (!sym.valid())
            {
                return;
            }
            auto [it, inserted] = view.symbolIndex_.emplace(sym.value, GraphView::SymbolBinding{kind, index});
            if (!inserted)
            {
                const char *owner = it->second.kind == GraphView::SymbolKind::kValue ? "value" : "operation";
                throw std::runtime_error(std::string(context) + " symbol already bound to " + owner);
            }
        };

        view.operations_.reserve(opCount);
        view.opKinds_.reserve(opCount);
        view.opSymbols_.reserve(opCount);
        view.opOperandRanges_.reserve(opCount);
        view.opResultRanges_.reserve(opCount);
        view.opAttrRanges_.reserve(opCount);
        view.opSrcLocs_.reserve(opCount);

        std::size_t operandOffset = 0;
        std::size_t resultOffset = 0;
        std::size_t attrOffset = 0;
        for (std::size_t i = 0; i < operations_.size(); ++i)
        {
            const OperationData &opData = operations_[i];
            if (!opData.alive)
            {
                continue;
            }
            OperationId opId;
            opId.index = opRemap[i + 1];
            opId.generation = 0;
            opId.graph = graphId_;

            view.operations_.push_back(opId);
            view.opKinds_.push_back(opData.kind);
            view.opSymbols_.push_back(opData.symbol);
            view.opSrcLocs_.push_back(opData.srcLoc);
            bindViewSymbol(opData.symbol, GraphView::SymbolKind::kOperation, opId.index, "Operation");

            view.opOperandRanges_.push_back(Range{operandOffset, opData.operands.size()});
            for (const auto &operand : opData.operands)
            {
                view.operands_.push_back(remapValue(operand));
            }
            operandOffset += opData.operands.size();

            view.opResultRanges_.push_back(Range{resultOffset, opData.results.size()});
            for (const auto &result : opData.results)
            {
                view.results_.push_back(remapValue(result));
            }
            resultOffset += opData.results.size();

            view.opAttrRanges_.push_back(Range{attrOffset, opData.attrs.size()});
            view.opAttrs_.insert(view.opAttrs_.end(), opData.attrs.begin(), opData.attrs.end());
            attrOffset += opData.attrs.size();
        }

        view.values_.reserve(valueCount);
        view.valueSymbols_.reserve(valueCount);
        view.valueWidths_.reserve(valueCount);
        view.valueSigned_.reserve(valueCount);
        view.valueTypes_.reserve(valueCount);
        view.valueIsInput_.reserve(valueCount);
        view.valueIsOutput_.reserve(valueCount);
        view.valueIsInout_.reserve(valueCount);
        view.valueDefs_.reserve(valueCount);
        view.valueUserRanges_.reserve(valueCount);
        view.valueSrcLocs_.reserve(valueCount);

        for (std::size_t i = 0; i < values_.size(); ++i)
        {
            const ValueData &valueData = values_[i];
            if (!valueData.alive)
            {
                continue;
            }
            ValueId valueId;
            valueId.index = valueRemap[i + 1];
            valueId.generation = 0;
            valueId.graph = graphId_;
            view.values_.push_back(valueId);
            view.valueSymbols_.push_back(valueData.symbol);
            view.valueWidths_.push_back(valueData.width);
            view.valueSigned_.push_back(valueData.isSigned ? 1 : 0);
            view.valueTypes_.push_back(static_cast<uint8_t>(valueData.type));
            view.valueIsInput_.push_back(valueData.isInput ? 1 : 0);
            view.valueIsOutput_.push_back(valueData.isOutput ? 1 : 0);
            view.valueIsInout_.push_back(valueData.isInout ? 1 : 0);
            view.valueSrcLocs_.push_back(valueData.srcLoc);
            bindViewSymbol(valueData.symbol, GraphView::SymbolKind::kValue, valueId.index, "Value");

            OperationId def = OperationId::invalid();
            if (valueData.definingOp.valid())
            {
                def = remapOp(valueData.definingOp);
            }
            view.valueDefs_.push_back(def);
        }

        view.inputPorts_.reserve(inputPorts_.size());
        for (const auto &port : inputPorts_)
        {
            Port mapped{port.name, remapValue(port.value)};
            view.inputPorts_.push_back(mapped);
        }
        std::sort(view.inputPorts_.begin(), view.inputPorts_.end(), portLess);

        view.outputPorts_.reserve(outputPorts_.size());
        for (const auto &port : outputPorts_)
        {
            Port mapped{port.name, remapValue(port.value)};
            view.outputPorts_.push_back(mapped);
        }
        std::sort(view.outputPorts_.begin(), view.outputPorts_.end(), portLess);

        view.inoutPorts_.reserve(inoutPorts_.size());
        for (const auto &port : inoutPorts_)
        {
            InoutPort mapped{port.name, remapValue(port.in), remapValue(port.out), remapValue(port.oe)};
            view.inoutPorts_.push_back(mapped);
        }
        std::sort(view.inoutPorts_.begin(), view.inoutPorts_.end(), inoutPortLess);

        std::vector<std::vector<ValueUser>> users(valueCount);
        for (std::size_t i = 0; i < operations_.size(); ++i)
        {
            const OperationData &opData = operations_[i];
            if (!opData.alive)
            {
                continue;
            }
            OperationId opId;
            opId.index = opRemap[i + 1];
            opId.generation = 0;
            opId.graph = graphId_;
            for (std::size_t operandIndex = 0; operandIndex < opData.operands.size(); ++operandIndex)
            {
                const ValueId valueId = remapValue(opData.operands[operandIndex]);
                const std::size_t valIdx = static_cast<std::size_t>(valueId.index - 1);
                users[valIdx].push_back(ValueUser{opId, static_cast<uint32_t>(operandIndex)});
            }
        }

        std::size_t userOffset = 0;
        for (const auto &bucket : users)
        {
            view.valueUserRanges_.push_back(Range{userOffset, bucket.size()});
            view.useList_.insert(view.useList_.end(), bucket.begin(), bucket.end());
            userOffset += bucket.size();
        }

        return view;
    }

    std::size_t GraphBuilder::valueIndex(ValueId value) const
    {
        value.assertGraph(graphId_);
        const std::size_t index = static_cast<std::size_t>(value.index);
        if (index == 0 || index > values_.size())
        {
            throw std::runtime_error("ValueId out of range");
        }
        return index - 1;
    }

    std::size_t GraphBuilder::opIndex(OperationId op) const
    {
        op.assertGraph(graphId_);
        const std::size_t index = static_cast<std::size_t>(op.index);
        if (index == 0 || index > operations_.size())
        {
            throw std::runtime_error("OperationId out of range");
        }
        return index - 1;
    }

    bool GraphBuilder::valueAlive(ValueId value) const
    {
        return values_[valueIndex(value)].alive;
    }

    bool GraphBuilder::opAlive(OperationId op) const
    {
        return operations_[opIndex(op)].alive;
    }

    std::size_t GraphBuilder::countValueUses(ValueId value, std::optional<OperationId> skipOp) const
    {
        const std::size_t valIdx = valueIndex(value);
        if (!skipOp)
        {
            return valueUsers_[valIdx].size();
        }
        skipOp->assertGraph(graphId_);
        std::size_t count = 0;
        for (const auto &user : valueUsers_[valIdx])
        {
            if (user.operation != *skipOp)
            {
                ++count;
            }
        }
        return count;
    }

    void GraphBuilder::addValueUser(ValueId value, OperationId op, std::size_t operandIndex)
    {
        const std::size_t valIdx = valueIndex(value);
        op.assertGraph(graphId_);
        if (!values_[valIdx].alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }
        valueUsers_[valIdx].push_back(ValueUser{op, static_cast<uint32_t>(operandIndex)});
    }

    void GraphBuilder::removeValueUser(ValueId value, OperationId op, std::size_t operandIndex)
    {
        const std::size_t valIdx = valueIndex(value);
        op.assertGraph(graphId_);
        auto &users = valueUsers_[valIdx];
        for (std::size_t i = 0; i < users.size(); ++i)
        {
            if (users[i].operation == op && users[i].operandIndex == operandIndex)
            {
                users.erase(users.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
        throw std::runtime_error("Value use-list out of sync while removing user");
    }

    void GraphBuilder::removeOpUses(OperationId op, const std::vector<ValueId> &operands)
    {
        for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
        {
            removeValueUser(operands[operandIndex], op, operandIndex);
        }
    }

    void GraphBuilder::addOpUses(OperationId op, const std::vector<ValueId> &operands)
    {
        for (std::size_t operandIndex = 0; operandIndex < operands.size(); ++operandIndex)
        {
            addValueUser(operands[operandIndex], op, operandIndex);
        }
    }

    void GraphBuilder::replaceAllUsesInternal(ValueId from, ValueId to, std::optional<OperationId> skipOp)
    {
        if (from == to)
        {
            return;
        }
        const std::size_t fromIdx = valueIndex(from);
        const std::size_t toIdx = valueIndex(to);
        if (!values_[fromIdx].alive || !values_[toIdx].alive)
        {
            throw std::runtime_error("replaceAllUses requires live values");
        }
        if (skipOp)
        {
            skipOp->assertGraph(graphId_);
        }

        std::vector<ValueUser> users = valueUsers_[fromIdx];
        valueUsers_[fromIdx].clear();
        valueUsers_[fromIdx].reserve(users.size());
        valueUsers_[toIdx].reserve(valueUsers_[toIdx].size() + users.size());

        for (const auto &user : users)
        {
            if (skipOp && user.operation == *skipOp)
            {
                valueUsers_[fromIdx].push_back(user);
                continue;
            }
            const std::size_t opIdx = opIndex(user.operation);
            if (!operations_[opIdx].alive)
            {
                continue;
            }
            auto &operands = operations_[opIdx].operands;
            if (user.operandIndex >= operands.size())
            {
                throw std::runtime_error("Value use-list out of sync with operands");
            }
            if (operands[user.operandIndex] != from)
            {
                throw std::runtime_error("Value use-list out of sync with operands");
            }
            operands[user.operandIndex] = to;
            valueUsers_[toIdx].push_back(ValueUser{user.operation, user.operandIndex});
        }
    }

    void GraphBuilder::recomputePortFlags()
    {
        for (auto &value : values_)
        {
            value.isInput = false;
            value.isOutput = false;
            value.isInout = false;
        }
        for (const auto &port : inputPorts_)
        {
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("Input port references erased value");
            }
            values_[valIdx].isInput = true;
        }
        for (const auto &port : outputPorts_)
        {
            const std::size_t valIdx = valueIndex(port.value);
            if (!values_[valIdx].alive)
            {
                throw std::runtime_error("Output port references erased value");
            }
            values_[valIdx].isOutput = true;
        }
        for (const auto &port : inoutPorts_)
        {
            auto markInout = [&](ValueId value)
            {
                const std::size_t valIdx = valueIndex(value);
                if (!values_[valIdx].alive)
                {
                    throw std::runtime_error("Inout port references erased value");
                }
                if (values_[valIdx].isInput || values_[valIdx].isOutput)
                {
                    throw std::runtime_error("Inout port references value bound as input/output");
                }
                values_[valIdx].isInout = true;
            };
            markInout(port.in);
            markInout(port.out);
            markInout(port.oe);
        }
    }

    void GraphBuilder::validateSymbol(SymbolId sym, std::string_view context) const
    {
        if (!sym.valid())
        {
            throw std::runtime_error(std::string(context) + " symbol is invalid");
        }
        if (symbols_ && !symbols_->valid(sym))
        {
            throw std::runtime_error(std::string(context) + " symbol is not in the symbol table");
        }
    }

    void GraphBuilder::bindSymbol(SymbolId sym, SymbolKind kind, uint32_t index, std::string_view context)
    {
        if (!sym.valid())
        {
            throw std::runtime_error(std::string(context) + " symbol is invalid");
        }
        auto [it, inserted] = symbolIndex_.emplace(sym.value, SymbolBinding{kind, index});
        if (!inserted)
        {
            if (it->second.kind == kind && it->second.index == index)
            {
                return;
            }
            const char *owner = it->second.kind == SymbolKind::kValue ? "value" : "operation";
            std::string message = std::string(context) + " symbol already bound to " + owner;
            if (symbols_ && symbols_->valid(sym))
            {
                std::string_view symbolText = symbols_->text(sym);
                if (!symbolText.empty())
                {
                    message.append(": ");
                    message.append(symbolText);
                }
            }
            throw std::runtime_error(message);
        }
    }

    void GraphBuilder::unbindSymbol(SymbolId sym, SymbolKind kind, uint32_t index)
    {
        if (!sym.valid())
        {
            return;
        }
        auto it = symbolIndex_.find(sym.value);
        if (it == symbolIndex_.end())
        {
            throw std::runtime_error("Symbol binding missing during unbind");
        }
        if (it->second.kind != kind || it->second.index != index)
        {
            throw std::runtime_error("Symbol binding mismatch during unbind");
        }
        symbolIndex_.erase(it);
    }

    Design::Design(Design &&other) noexcept
    {
        *this = std::move(other);
    }

    Design &Design::operator=(Design &&other) noexcept
    {
        if (this != &other)
        {
            graphs_ = std::move(other.graphs_);
            graphAliasBySymbol_ = std::move(other.graphAliasBySymbol_);
            graphOrder_ = std::move(other.graphOrder_);
            topGraphs_ = std::move(other.topGraphs_);
            declaredSymbols_ = std::move(other.declaredSymbols_);
            declaredSymbolSet_ = std::move(other.declaredSymbolSet_);
            designSymbols_ = std::move(other.designSymbols_);
            resetGraphOwners();

            other.graphAliasBySymbol_.clear();
            other.graphOrder_.clear();
            other.topGraphs_.clear();
            other.declaredSymbols_.clear();
            other.declaredSymbolSet_.clear();
        }
        return *this;
    }

    void Design::resetGraphOwners()
    {
        for (auto &entry : graphs_)
        {
            if (entry.second)
            {
                entry.second->owner_ = this;
            }
        }
    }

    namespace
    {

        template <class... Ts>
        struct Overloaded : Ts...
        {
            using Ts::operator()...;
        };
        template <class... Ts>
        Overloaded(Ts...) -> Overloaded<Ts...>;

        constexpr std::string_view kOperationNames[] = {
            "kConstant",
            "kAdd",
            "kSub",
            "kMul",
            "kDiv",
            "kMod",
            "kEq",
            "kNe",
            "kCaseEq",
            "kCaseNe",
            "kWildcardEq",
            "kWildcardNe",
            "kLt",
            "kLe",
            "kGt",
            "kGe",
            "kAnd",
            "kOr",
            "kXor",
            "kXnor",
            "kNot",
            "kLogicAnd",
            "kLogicOr",
            "kLogicNot",
            "kReduceAnd",
            "kReduceOr",
            "kReduceXor",
            "kReduceNor",
            "kReduceNand",
            "kReduceXnor",
            "kShl",
            "kLShr",
            "kAShr",
            "kMux",
            "kAssign",
            "kConcat",
            "kReplicate",
            "kSliceStatic",
            "kSliceDynamic",
            "kSliceArray",
            "kLatch",
            "kLatchReadPort",
            "kLatchWritePort",
            "kRegister",
            "kRegisterReadPort",
            "kRegisterWritePort",
            "kMemory",
            "kMemoryReadPort",
            "kMemoryWritePort",
            "kMemoryFillPort",
            "kInstance",
            "kBlackbox",
            "kSystemFunction",
            "kSystemTask",
            "kDpicImport",
            "kDpicCall",
            "kXMRRead",
            "kXMRWrite"};

        void writeAttributeValue(slang::JsonWriter &writer, const AttributeValue &value)
        {
            if (!attributeValueIsJsonSerializable(value))
            {
                throw std::runtime_error("Attribute value is not JSON serializable");
            }
            std::visit(
                Overloaded{
                    [&](bool v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("bool"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](int64_t v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("int"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](double v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("double"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](const std::string &v)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("string"));
                        writer.writeProperty("v");
                        writer.writeValue(v);
                    },
                    [&](const std::vector<bool> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("bool[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (bool entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    },
                    [&](const std::vector<int64_t> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("int[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (int64_t entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    },
                    [&](const std::vector<double> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("double[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (double entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    },
                    [&](const std::vector<std::string> &arr)
                    {
                        writer.writeProperty("t");
                        writer.writeValue(std::string_view("string[]"));
                        writer.writeProperty("vs");
                        writer.startArray();
                        for (const auto &entry : arr)
                        {
                            writer.writeValue(entry);
                        }
                        writer.endArray();
                    }},
                value);
        }

        void writeSrcLoc(slang::JsonWriter &writer, const std::optional<SrcLoc> &srcLoc)
        {
            if (!srcLoc)
            {
                return;
            }
            if (srcLoc->file.empty() && srcLoc->line == 0 && srcLoc->column == 0 &&
                srcLoc->endLine == 0 && srcLoc->endColumn == 0 &&
                srcLoc->origin.empty() && srcLoc->pass.empty() && srcLoc->note.empty())
            {
                return;
            }
            writer.writeProperty("loc");
            writer.startObject();
            if (!srcLoc->file.empty())
            {
                writer.writeProperty("file");
                writer.writeValue(srcLoc->file);
            }
            if (srcLoc->line != 0)
            {
                writer.writeProperty("line");
                writer.writeValue(static_cast<int64_t>(srcLoc->line));
            }
            if (srcLoc->column != 0)
            {
                writer.writeProperty("col");
                writer.writeValue(static_cast<int64_t>(srcLoc->column));
            }
            if (srcLoc->endLine != 0)
            {
                writer.writeProperty("endLine");
                writer.writeValue(static_cast<int64_t>(srcLoc->endLine));
            }
            if (srcLoc->endColumn != 0)
            {
                writer.writeProperty("endCol");
                writer.writeValue(static_cast<int64_t>(srcLoc->endColumn));
            }
            if (!srcLoc->origin.empty())
            {
                writer.writeProperty("origin");
                writer.writeValue(srcLoc->origin);
            }
            if (!srcLoc->pass.empty())
            {
                writer.writeProperty("pass");
                writer.writeValue(srcLoc->pass);
            }
            if (!srcLoc->note.empty())
            {
                writer.writeProperty("note");
                writer.writeValue(srcLoc->note);
            }
            writer.endObject();
        }

    } // namespace

    std::string_view toString(OperationKind kind) noexcept
    {
        auto index = static_cast<std::size_t>(kind);
        if (index >= std::size(kOperationNames))
        {
            return "<unknown>";
        }
        return kOperationNames[index];
    }

    std::optional<OperationKind> parseOperationKind(std::string_view text) noexcept
    {
        for (std::size_t i = 0; i < std::size(kOperationNames); ++i)
        {
            if (kOperationNames[i] == text)
            {
                return static_cast<OperationKind>(i);
            }
        }
        return std::nullopt;
    }

    std::string_view toString(ValueType type) noexcept
    {
        switch (type)
        {
        case ValueType::Logic:
            return "logic";
        case ValueType::Real:
            return "real";
        case ValueType::String:
            return "string";
        default:
            return "<unknown>";
        }
    }

    std::optional<ValueType> parseValueType(std::string_view text) noexcept
    {
        if (text == "logic")
        {
            return ValueType::Logic;
        }
        if (text == "real")
        {
            return ValueType::Real;
        }
        if (text == "string")
        {
            return ValueType::String;
        }
        return std::nullopt;
    }

    Value::Value(ValueId id, SymbolId symbol, std::string symbolText, int32_t width, bool isSigned,
                 ValueType type, bool isInput, bool isOutput, bool isInout,
                 OperationId definingOp, std::span<const ValueUser> users,
                 std::optional<SrcLoc> srcLoc, const Graph *owner, bool usersLoaded)
        : id_(id),
          symbol_(symbol),
          symbolText_(std::move(symbolText)),
          width_(width),
          isSigned_(isSigned),
          type_(type),
          isInput_(isInput),
          isOutput_(isOutput),
          isInout_(isInout),
          definingOp_(definingOp),
          usersLoaded_(usersLoaded),
          owner_(owner),
          srcLoc_(std::move(srcLoc))
    {
        if (usersLoaded_)
        {
            users_.assign(users);
        }
    }

    std::span<const ValueUser> Value::users() const noexcept
    {
        if (!usersLoaded_)
        {
            if (owner_ != nullptr)
            {
                users_.assign(owner_->valueUsersSpan(id_));
            }
            usersLoaded_ = true;
        }
        return users_.span();
    }

    Operation::Operation(OperationId id, OperationKind kind, SymbolId symbol, std::string symbolText,
                         std::vector<ValueId> operands, std::vector<ValueId> results,
                         std::vector<AttrKV> attrs, std::optional<SrcLoc> srcLoc)
        : id_(id),
          kind_(kind),
          symbol_(symbol),
          symbolText_(std::move(symbolText)),
          operands_(std::move(operands)),
          results_(std::move(results)),
          attrs_(std::move(attrs)),
          srcLoc_(std::move(srcLoc))
    {
    }

    std::optional<AttributeValue> Operation::attr(std::string_view key) const
    {
        for (const auto &entry : attrs_)
        {
            if (entry.key == key)
            {
                return entry.value;
            }
        }
        return std::nullopt;
    }

    Graph::Graph(Design &owner, std::string symbol, GraphId graphId)
        : owner_(&owner),
          symbol_(std::move(symbol)),
          graphId_(graphId)
    {
        if (symbol_.empty())
        {
            throw std::invalid_argument("Graph symbol must not be empty");
        }
        if (!graphId_.valid())
        {
            throw std::invalid_argument("GraphId must be valid");
        }
        GraphBuilder builder(symbols_, graphId_);
        view_ = builder.freeze();
    }

    SymbolId Graph::internSymbol(std::string_view text)
    {
        SymbolId existing = symbols_.lookup(text);
        if (existing.valid())
        {
            if (builder_)
            {
                if (builder_->symbolIndex_.contains(existing.value))
                {
                    return SymbolId::invalid();
                }
                return existing;
            }
            if (findValue(existing).valid() || findOperation(existing).valid())
            {
                return SymbolId::invalid();
            }
            return existing;
        }
        return symbols_.intern(text);
    }

    SymbolId Graph::lookupSymbol(std::string_view text) const
    {
        return symbols_.lookup(text);
    }

    std::string_view Graph::symbolText(SymbolId id) const
    {
        if (!id.valid())
        {
            return std::string_view{};
        }
        return symbols_.text(id);
    }

    SymbolId Graph::makeInternalOpSym()
    {
        std::string base = Graph::makeInternalBase("op");
        for (;;)
        {
            std::string candidate = base;
            candidate.push_back('_');
            candidate.append(std::to_string(nextInternalOpSym_++));
            if (!symbols_.contains(candidate))
            {
                SymbolId sym = internSymbol(candidate);
                if (sym.valid())
                {
                    return sym;
                }
            }
        }
    }

    SymbolId Graph::makeInternalValSym()
    {
        std::string base = Graph::makeInternalBase("val");
        for (;;)
        {
            std::string candidate = base;
            candidate.push_back('_');
            candidate.append(std::to_string(nextInternalValSym_++));
            if (!symbols_.contains(candidate))
            {
                SymbolId sym = internSymbol(candidate);
                if (sym.valid())
                {
                    return sym;
                }
            }
        }
    }

    void Graph::addDeclaredSymbol(SymbolId sym)
    {
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!symbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the graph symbol table");
        }
        if (declaredSymbolSet_.insert(sym.value).second)
        {
            declaredSymbols_.push_back(sym);
        }
    }

    bool Graph::removeDeclaredSymbol(SymbolId sym)
    {
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!symbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the graph symbol table");
        }
        if (declaredSymbolSet_.erase(sym.value) == 0)
        {
            return false;
        }
        auto it = std::remove_if(declaredSymbols_.begin(), declaredSymbols_.end(),
                                 [&](SymbolId entry) { return entry == sym; });
        if (it != declaredSymbols_.end())
        {
            declaredSymbols_.erase(it, declaredSymbols_.end());
        }
        return true;
    }

    void Graph::clearDeclaredSymbols()
    {
        declaredSymbols_.clear();
        declaredSymbolSet_.clear();
    }

    bool Graph::isDeclaredSymbol(SymbolId sym) const noexcept
    {
        if (!sym.valid())
        {
            return false;
        }
        return declaredSymbolSet_.find(sym.value) != declaredSymbolSet_.end();
    }

    std::span<const SymbolId> Graph::declaredSymbols() const noexcept
    {
        return std::span<const SymbolId>(declaredSymbols_.data(), declaredSymbols_.size());
    }

    std::size_t Graph::addGenerateGroup(SymbolId scope, SymbolId name)
    {
        if (!scope.valid() || !name.valid())
        {
            throw std::runtime_error("Generate group scope/name symbol is invalid");
        }
        if (!symbols_.valid(scope) || !symbols_.valid(name))
        {
            throw std::runtime_error(
                "Generate group scope/name symbol is not in the graph symbol table");
        }
        generateGroups_.push_back(GenerateGroup{scope, name, {}});
        return generateGroups_.size() - 1;
    }

    void Graph::addGenerateGroupSymbol(std::size_t group, SymbolId symbol)
    {
        if (group >= generateGroups_.size())
        {
            throw std::runtime_error("Generate group index out of range");
        }
        if (!symbol.valid())
        {
            throw std::runtime_error("Generate group symbol is invalid");
        }
        if (!symbols_.valid(symbol))
        {
            throw std::runtime_error("Generate group symbol is not in the graph symbol table");
        }
        generateGroups_[group].symbols.push_back(symbol);
    }

    const std::vector<Graph::GenerateGroup>& Graph::generateGroups() const noexcept
    {
        return generateGroups_;
    }

    std::vector<std::string> Graph::validateDeclaredSymbols() const
    {
        std::vector<std::string> issues;
        auto memberText = [&](SymbolId sym) -> std::string
        {
            if (sym.valid() && symbols_.valid(sym))
            {
                return std::string(symbolText(sym));
            }
            return std::string("<invalid>");
        };
        auto checkMember = [&](SymbolId sym, std::string_view prefix)
        {
            if (!sym.valid() || !symbols_.valid(sym) ||
                (!findValue(sym).valid() && !findOperation(sym).valid()))
            {
                issues.push_back(std::string(prefix) + memberText(sym));
            }
        };
        for (const SymbolId sym : declaredSymbols_)
        {
            checkMember(sym, "declared:");
        }
        for (const GenerateGroup& group : generateGroups_)
        {
            if (!group.scope.valid() || !symbols_.valid(group.scope))
            {
                issues.push_back("generateGroupScope:" + memberText(group.scope));
            }
            if (!group.name.valid() || !symbols_.valid(group.name))
            {
                issues.push_back("generateGroupName:" + memberText(group.name));
            }
            for (const SymbolId sym : group.symbols)
            {
                checkMember(sym, "generateGroup:");
            }
        }
        return issues;
    }

    void Graph::removeSymbolFromGenerateGroups(SymbolId sym) noexcept
    {
        if (!sym.valid() || generateGroups_.empty())
        {
            return;
        }
        for (GenerateGroup& group : generateGroups_)
        {
            std::vector<SymbolId>& symbols = group.symbols;
            symbols.erase(std::remove(symbols.begin(), symbols.end(), sym), symbols.end());
        }
    }

    void Graph::freeze()
    {
        if (builder_)
        {
            view_ = builder_->freeze();
            builder_.reset();
            invalidateCaches();
        }
        if (!view_)
        {
            GraphBuilder builder(symbols_, graphId_);
            view_ = builder.freeze();
        }
    }

    std::span<const OperationId> Graph::operations() const
    {
        if (!builder_)
        {
            if (view_)
            {
                return view_->operations();
            }
            return std::span<const OperationId>();
        }
        ensureOperationsCache();
        return std::span<const OperationId>(operationsCache_.data(), operationsCache_.size());
    }

    std::span<const ValueId> Graph::values() const
    {
        if (!builder_)
        {
            if (view_)
            {
                return view_->values();
            }
            return std::span<const ValueId>();
        }
        ensureValuesCache();
        return std::span<const ValueId>(valuesCache_.data(), valuesCache_.size());
    }

    std::span<const Port> Graph::inputPorts() const
    {
        if (!builder_)
        {
            if (view_)
            {
                return view_->inputPorts();
            }
            return std::span<const Port>();
        }
        ensurePortsCache();
        return std::span<const Port>(inputPortsCache_.data(), inputPortsCache_.size());
    }

    std::span<const Port> Graph::outputPorts() const
    {
        if (!builder_)
        {
            if (view_)
            {
                return view_->outputPorts();
            }
            return std::span<const Port>();
        }
        ensurePortsCache();
        return std::span<const Port>(outputPortsCache_.data(), outputPortsCache_.size());
    }

    std::span<const InoutPort> Graph::inoutPorts() const
    {
        if (!builder_)
        {
            if (view_)
            {
                return view_->inoutPorts();
            }
            return std::span<const InoutPort>();
        }
        ensurePortsCache();
        return std::span<const InoutPort>(inoutPortsCache_.data(), inoutPortsCache_.size());
    }

    ValueId Graph::createValue(SymbolId symbol, int32_t width, bool isSigned, ValueType type)
    {
        GraphBuilder &builder = ensureBuilder();
        ValueId id = builder.addValue(symbol, width, isSigned, type);
        // Incremental update: directly append to cache without full rebuild
        if (!valuesCacheDirty_)
        {
            valuesCache_.push_back(id);
        }
        return id;
    }

    ValueId Graph::createValue(int32_t width, bool isSigned, ValueType type)
    {
        return createValue(makeInternalValSym(), width, isSigned, type);
    }

    OperationId Graph::createOperation(OperationKind kind, SymbolId symbol)
    {
        GraphBuilder &builder = ensureBuilder();
        OperationId id = builder.addOp(kind, symbol);
        // Incremental update: directly append to cache without full rebuild
        if (!operationsCacheDirty_)
        {
            operationsCache_.push_back(id);
        }
        return id;
    }

    OperationId Graph::createOperation(OperationKind kind)
    {
        return createOperation(kind, makeInternalOpSym());
    }

    ValueId Graph::findValue(SymbolId symbol) const noexcept
    {
        if (!symbol.valid())
        {
            return ValueId::invalid();
        }
        if (builder_)
        {
            auto it = builder_->symbolIndex_.find(symbol.value);
            if (it == builder_->symbolIndex_.end() || it->second.kind != GraphBuilder::SymbolKind::kValue)
            {
                return ValueId::invalid();
            }
            ValueId id;
            id.index = it->second.index;
            id.generation = 0;
            id.graph = graphId_;
            return id;
        }
        if (!view_)
        {
            return ValueId::invalid();
        }
        return view_->findValue(symbol);
    }

    OperationId Graph::findOperation(SymbolId symbol) const noexcept
    {
        if (!symbol.valid())
        {
            return OperationId::invalid();
        }
        if (builder_)
        {
            auto it = builder_->symbolIndex_.find(symbol.value);
            if (it == builder_->symbolIndex_.end() || it->second.kind != GraphBuilder::SymbolKind::kOperation)
            {
                return OperationId::invalid();
            }
            OperationId id;
            id.index = it->second.index;
            id.generation = 0;
            id.graph = graphId_;
            return id;
        }
        if (!view_)
        {
            return OperationId::invalid();
        }
        return view_->findOperation(symbol);
    }

    ValueId Graph::findValue(std::string_view symbol) const
    {
        return findValue(lookupSymbol(symbol));
    }

    OperationId Graph::findOperation(std::string_view symbol) const
    {
        return findOperation(lookupSymbol(symbol));
    }

    SymbolId Graph::valueSymbol(ValueId id) const noexcept
    {
        if (builder_)
        {
            if (id.graph != graphId_ || id.index == 0 || id.index > builder_->values_.size())
            {
                return SymbolId::invalid();
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            if (!builder_->values_[idx].alive)
            {
                return SymbolId::invalid();
            }
            return builder_->values_[idx].symbol;
        }
        if (view_)
        {
            return view_->valueSymbol(id);
        }
        return SymbolId::invalid();
    }

    SymbolId Graph::operationSymbol(OperationId id) const noexcept
    {
        if (builder_)
        {
            if (id.graph != graphId_ || id.index == 0 || id.index > builder_->operations_.size())
            {
                return SymbolId::invalid();
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            if (!builder_->operations_[idx].alive)
            {
                return SymbolId::invalid();
            }
            return builder_->operations_[idx].symbol;
        }
        if (view_)
        {
            return view_->opSymbol(id);
        }
        return SymbolId::invalid();
    }

    int32_t Graph::valueWidth(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.width;
        }
        if (view_)
        {
            return view_->valueWidth(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    bool Graph::valueSigned(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.isSigned;
        }
        if (view_)
        {
            return view_->valueSigned(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    ValueType Graph::valueType(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.type;
        }
        if (view_)
        {
            return view_->valueType(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    bool Graph::valueIsInput(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.isInput;
        }
        if (view_)
        {
            return view_->valueIsInput(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    bool Graph::valueIsOutput(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.isOutput;
        }
        if (view_)
        {
            return view_->valueIsOutput(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    bool Graph::valueIsInout(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.isInout;
        }
        if (view_)
        {
            return view_->valueIsInout(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    OperationId Graph::valueDef(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.definingOp;
        }
        if (view_)
        {
            return view_->valueDef(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    std::optional<SrcLoc> Graph::valueSrcLoc(ValueId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->values_.size())
            {
                throw std::runtime_error("ValueId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->values_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("ValueId refers to erased value");
            }
            return data.srcLoc;
        }
        if (view_)
        {
            return view_->valueSrcLoc(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    OperationKind Graph::opKind(OperationId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->operations_.size())
            {
                throw std::runtime_error("OperationId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->operations_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("OperationId refers to erased operation");
            }
            return data.kind;
        }
        if (view_)
        {
            return view_->opKind(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    std::span<const ValueId> Graph::opOperands(OperationId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->operations_.size())
            {
                throw std::runtime_error("OperationId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->operations_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("OperationId refers to erased operation");
            }
            return std::span<const ValueId>(data.operands.data(), data.operands.size());
        }
        if (view_)
        {
            return view_->opOperands(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    std::span<const ValueId> Graph::opResults(OperationId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->operations_.size())
            {
                throw std::runtime_error("OperationId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->operations_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("OperationId refers to erased operation");
            }
            return std::span<const ValueId>(data.results.data(), data.results.size());
        }
        if (view_)
        {
            return view_->opResults(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    std::span<const AttrKV> Graph::opAttrs(OperationId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->operations_.size())
            {
                throw std::runtime_error("OperationId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->operations_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("OperationId refers to erased operation");
            }
            return std::span<const AttrKV>(data.attrs.data(), data.attrs.size());
        }
        if (view_)
        {
            return view_->opAttrs(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    std::optional<SrcLoc> Graph::opSrcLoc(OperationId id) const
    {
        if (builder_)
        {
            id.assertGraph(graphId_);
            if (id.index == 0 || id.index > builder_->operations_.size())
            {
                throw std::runtime_error("OperationId out of range");
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            const auto &data = builder_->operations_[idx];
            if (!data.alive)
            {
                throw std::runtime_error("OperationId refers to erased operation");
            }
            return data.srcLoc;
        }
        if (view_)
        {
            return view_->opSrcLoc(id);
        }
        throw std::runtime_error("GraphView is not available; freeze the graph first");
    }

    Value Graph::getValue(ValueId id) const
    {
        if (builder_)
        {
            return valueFromBuilder(id);
        }
        return valueFromView(id);
    }

    Operation Graph::getOperation(OperationId id) const
    {
        if (builder_)
        {
            return operationFromBuilder(id);
        }
        return operationFromView(id);
    }

    void Graph::bindInputPort(std::string_view name, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.bindInputPort(name, value);
        // Incremental update: sync cache directly
        if (!portsCacheDirty_)
        {
            inputPortsCache_.clear();
            inputPortsCache_ = builder.inputPorts_;
        }
    }

    void Graph::bindOutputPort(std::string_view name, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.bindOutputPort(name, value);
        // Incremental update: sync cache directly
        if (!portsCacheDirty_)
        {
            outputPortsCache_.clear();
            outputPortsCache_ = builder.outputPorts_;
        }
    }

    void Graph::bindInoutPort(std::string_view name, ValueId in, ValueId out, ValueId oe)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.bindInoutPort(name, in, out, oe);
        // Incremental update: sync cache directly
        if (!portsCacheDirty_)
        {
            inoutPortsCache_.clear();
            inoutPortsCache_ = builder.inoutPorts_;
        }
    }

    void Graph::bindInputPorts(std::span<const Port> ports)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.bindInputPorts(ports);
        if (!portsCacheDirty_)
        {
            inputPortsCache_.clear();
            inputPortsCache_ = builder.inputPorts_;
        }
    }

    void Graph::bindOutputPorts(std::span<const Port> ports)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.bindOutputPorts(ports);
        if (!portsCacheDirty_)
        {
            outputPortsCache_.clear();
            outputPortsCache_ = builder.outputPorts_;
        }
    }

    void Graph::bindInoutPorts(std::span<const InoutPort> ports)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.bindInoutPorts(ports);
        if (!portsCacheDirty_)
        {
            inoutPortsCache_.clear();
            inoutPortsCache_ = builder.inoutPorts_;
        }
    }

    bool Graph::removeInputPort(std::string_view name)
    {
        GraphBuilder &builder = ensureBuilder();
        bool removed = builder.removeInputPort(name);
        if (removed && !portsCacheDirty_)
        {
            inputPortsCache_.clear();
            inputPortsCache_ = builder.inputPorts_;
        }
        return removed;
    }

    bool Graph::removeOutputPort(std::string_view name)
    {
        GraphBuilder &builder = ensureBuilder();
        bool removed = builder.removeOutputPort(name);
        if (removed && !portsCacheDirty_)
        {
            outputPortsCache_.clear();
            outputPortsCache_ = builder.outputPorts_;
        }
        return removed;
    }

    bool Graph::removeInoutPort(std::string_view name)
    {
        GraphBuilder &builder = ensureBuilder();
        bool removed = builder.removeInoutPort(name);
        if (removed && !portsCacheDirty_)
        {
            inoutPortsCache_.clear();
            inoutPortsCache_ = builder.inoutPorts_;
        }
        return removed;
    }

    ValueId Graph::inputPortValue(std::string_view name) const noexcept
    {
        if (builder_)
        {
            return findPortValue(std::span<const Port>(builder_->inputPorts_.data(),
                                                       builder_->inputPorts_.size()),
                                 name);
        }
        if (!view_)
        {
            return ValueId::invalid();
        }
        return findPortValue(view_->inputPorts(), name);
    }

    ValueId Graph::outputPortValue(std::string_view name) const noexcept
    {
        if (builder_)
        {
            return findPortValue(std::span<const Port>(builder_->outputPorts_.data(),
                                                       builder_->outputPorts_.size()),
                                 name);
        }
        if (!view_)
        {
            return ValueId::invalid();
        }
        return findPortValue(view_->outputPorts(), name);
    }

    void Graph::addOperand(OperationId op, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.addOperand(op, value);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::addResult(OperationId op, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.addResult(op, value);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::insertOperand(OperationId op, std::size_t index, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.insertOperand(op, index, value);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::insertResult(OperationId op, std::size_t index, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.insertResult(op, index, value);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::replaceOperand(OperationId op, std::size_t index, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.replaceOperand(op, index, value);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::replaceResult(OperationId op, std::size_t index, ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.replaceResult(op, index, value);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::replaceAllUses(ValueId from, ValueId to)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.replaceAllUses(from, to);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    bool Graph::eraseOperand(OperationId op, std::size_t index)
    {
        GraphBuilder &builder = ensureBuilder();
        return builder.eraseOperand(op, index);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    bool Graph::eraseResult(OperationId op, std::size_t index)
    {
        GraphBuilder &builder = ensureBuilder();
        return builder.eraseResult(op, index);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    bool Graph::eraseOp(OperationId op)
    {
        GraphBuilder &builder = ensureBuilder();
        SymbolId declaredSymbol;
        if (builder.opAlive(op))
        {
            const std::size_t opIdx = builder.opIndex(op);
            declaredSymbol = builder.operations_[opIdx].symbol;
        }
        bool result = builder.eraseOp(op);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);
            invalidateOperationsCache();
        }
        return result;
    }

    bool Graph::eraseOp(OperationId op, std::span<const ValueId> replacementResults)
    {
        GraphBuilder &builder = ensureBuilder();
        SymbolId declaredSymbol;
        if (builder.opAlive(op))
        {
            const std::size_t opIdx = builder.opIndex(op);
            declaredSymbol = builder.operations_[opIdx].symbol;
        }
        bool result = builder.eraseOp(op, replacementResults);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);
            invalidateOperationsCache();
        }
        return result;
    }

    bool Graph::eraseOpUnchecked(OperationId op)
    {
        GraphBuilder &builder = ensureBuilder();
        SymbolId declaredSymbol;
        if (builder.opAlive(op))
        {
            const std::size_t opIdx = builder.opIndex(op);
            declaredSymbol = builder.operations_[opIdx].symbol;
        }
        bool result = builder.eraseOpUnchecked(op);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);
            invalidateOperationsCache();
        }
        return result;
    }

    bool Graph::eraseValue(ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        SymbolId declaredSymbol;
        if (builder.valueAlive(value))
        {
            const std::size_t valIdx = builder.valueIndex(value);
            declaredSymbol = builder.values_[valIdx].symbol;
        }
        bool result = builder.eraseValue(value);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);
            invalidateValuesCache();
        }
        return result;
    }

    bool Graph::eraseValueUnchecked(ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        SymbolId declaredSymbol;
        if (builder.valueAlive(value))
        {
            const std::size_t valIdx = builder.valueIndex(value);
            declaredSymbol = builder.values_[valIdx].symbol;
        }
        bool result = builder.eraseValueUnchecked(value);
        if (result)
        {
            if (declaredSymbol.valid() && isDeclaredSymbol(declaredSymbol))
            {
                removeDeclaredSymbol(declaredSymbol);
            }
            removeSymbolFromGenerateGroups(declaredSymbol);
            invalidateValuesCache();
        }
        return result;
    }

    void Graph::setAttr(OperationId op, std::string_view key, AttributeValue value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.setAttr(op, key, std::move(value));
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::setOpKind(OperationId op, OperationKind kind)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.setOpKind(op, kind);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    bool Graph::eraseAttr(OperationId op, std::string_view key)
    {
        GraphBuilder &builder = ensureBuilder();
        return builder.eraseAttr(op, key);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::setValueSrcLoc(ValueId value, SrcLoc loc)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.setValueSrcLoc(value, std::move(loc));
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::setOpSrcLoc(OperationId op, SrcLoc loc)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.setOpSrcLoc(op, std::move(loc));
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::setOpSymbol(OperationId op, SymbolId sym)
    {
        // Only rebinds the op's symbol. declaredSymbols_/generateGroups_
        // membership is keyed on SymbolId and is intentionally not updated:
        // symbol transplantation (handing the old SymbolId to an equivalent
        // new entity) preserves membership naturally, while a plain rename
        // leaves the old SymbolId's membership untouched.
        GraphBuilder &builder = ensureBuilder();
        builder.setOpSymbol(op, sym);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::setValueSymbol(ValueId value, SymbolId sym)
    {
        // Only rebinds the value's symbol; same declaredSymbols_/generateGroups_
        // membership contract as setOpSymbol.
        GraphBuilder &builder = ensureBuilder();
        builder.setValueSymbol(value, sym);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::clearOpSymbol(OperationId op)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.clearOpSymbol(op);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::clearValueSymbol(ValueId value)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.clearValueSymbol(value);
        // No cache invalidation needed - doesn't affect value/op/port lists
    }

    void Graph::writeJson(slang::JsonWriter &writer) const
    {
        auto requireSymbolText = [&](SymbolId sym, std::string_view context) -> std::string_view
        {
            if (!sym.valid())
            {
                throw std::runtime_error(std::string(context) + " symbol is invalid");
            }
            if (!symbols_.valid(sym))
            {
                throw std::runtime_error(std::string(context) + " symbol is not in the symbol table");
            }
            std::string_view text = symbols_.text(sym);
            if (text.empty())
            {
                throw std::runtime_error(std::string(context) + " symbol is empty");
            }
            return text;
        };
        auto requirePortName = [&](std::string_view name, std::string_view context) -> std::string_view
        {
            if (name.empty())
            {
                throw std::runtime_error(std::string(context) + " name is empty");
            }
            return name;
        };

        writer.startObject();
        writer.writeProperty("symbol");
        if (symbol_.empty())
        {
            throw std::runtime_error("Graph symbol is empty");
        }
        writer.writeValue(symbol_);

        writer.writeProperty("declaredSymbols");
        writer.startArray();
        for (const auto sym : declaredSymbols_)
        {
            writer.writeValue(requireSymbolText(sym, "Declared symbol"));
        }
        writer.endArray();

        if (!generateGroups_.empty())
        {
            writer.writeProperty("generateGroups");
            writer.startArray();
            for (const GenerateGroup &group : generateGroups_)
            {
                writer.startObject();
                writer.writeProperty("scope");
                writer.writeValue(requireSymbolText(group.scope, "Generate group scope"));
                writer.writeProperty("name");
                writer.writeValue(requireSymbolText(group.name, "Generate group name"));
                writer.writeProperty("symbols");
                writer.startArray();
                for (const auto sym : group.symbols)
                {
                    writer.writeValue(requireSymbolText(sym, "Generate group symbol"));
                }
                writer.endArray();
                writer.endObject();
            }
            writer.endArray();
        }

        writer.writeProperty("vals");
        writer.startArray();
        for (const auto &valueId : values())
        {
            const Value value = getValue(valueId);
            writer.startObject();
            writer.writeProperty("sym");
            writer.writeValue(requireSymbolText(value.symbol(), "Value"));
            writer.writeProperty("w");
            writer.writeValue(static_cast<int64_t>(value.width()));
            writer.writeProperty("sgn");
            writer.writeValue(value.isSigned());
            writer.writeProperty("type");
            writer.writeValue(std::string(toString(value.type())));
            writer.writeProperty("in");
            writer.writeValue(value.isInput());
            writer.writeProperty("out");
            writer.writeValue(value.isOutput());
            writer.writeProperty("inout");
            writer.writeValue(value.isInout());
            if (value.definingOp().valid())
            {
                Operation defOp = getOperation(value.definingOp());
                writer.writeProperty("def");
                writer.writeValue(requireSymbolText(defOp.symbol(), "Operation"));
            }

            writer.writeProperty("users");
            writer.startArray();
            for (const auto &user : value.users())
            {
                writer.startObject();
                writer.writeProperty("op");
                Operation userOp = getOperation(user.operation);
                writer.writeValue(requireSymbolText(userOp.symbol(), "Operation"));
                writer.writeProperty("idx");
                writer.writeValue(static_cast<int64_t>(user.operandIndex));
                writer.endObject();
            }
            writer.endArray();
            writeSrcLoc(writer, value.srcLoc());
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("ports");
        writer.startObject();

        writer.writeProperty("in");
        writer.startArray();
        for (const auto &port : inputPorts())
        {
            writer.startObject();
            writer.writeProperty("name");
            writer.writeValue(requirePortName(port.name, "Input port"));
            writer.writeProperty("val");
            Value value = getValue(port.value);
            writer.writeValue(requireSymbolText(value.symbol(), "Value"));
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("out");
        writer.startArray();
        for (const auto &port : outputPorts())
        {
            writer.startObject();
            writer.writeProperty("name");
            writer.writeValue(requirePortName(port.name, "Output port"));
            writer.writeProperty("val");
            Value value = getValue(port.value);
            writer.writeValue(requireSymbolText(value.symbol(), "Value"));
            writer.endObject();
        }
        writer.endArray();

        writer.writeProperty("inout");
        writer.startArray();
        for (const auto &port : inoutPorts())
        {
            writer.startObject();
            writer.writeProperty("name");
            writer.writeValue(requirePortName(port.name, "Inout port"));
            writer.writeProperty("in");
            writer.writeValue(requireSymbolText(getValue(port.in).symbol(), "Value"));
            writer.writeProperty("out");
            writer.writeValue(requireSymbolText(getValue(port.out).symbol(), "Value"));
            writer.writeProperty("oe");
            writer.writeValue(requireSymbolText(getValue(port.oe).symbol(), "Value"));
            writer.endObject();
        }
        writer.endArray();

        writer.endObject(); // ports

        writer.writeProperty("ops");
        writer.startArray();
        for (const auto &opId : operations())
        {
            const Operation op = getOperation(opId);
            writer.startObject();
            writer.writeProperty("sym");
            writer.writeValue(requireSymbolText(op.symbol(), "Operation"));
            writer.writeProperty("kind");
            writer.writeValue(toString(op.kind()));

            writer.writeProperty("in");
            writer.startArray();
            for (const auto &operand : op.operands())
            {
                Value value = getValue(operand);
                writer.writeValue(requireSymbolText(value.symbol(), "Value"));
            }
            writer.endArray();

            writer.writeProperty("out");
            writer.startArray();
            for (const auto &result : op.results())
            {
                Value value = getValue(result);
                writer.writeValue(requireSymbolText(value.symbol(), "Value"));
            }
            writer.endArray();

            if (!op.attrs().empty())
            {
                writer.writeProperty("attrs");
                writer.startObject();
                for (const auto &attr : op.attrs())
                {
                    writer.writeProperty(attr.key);
                    writer.startObject();
                    writeAttributeValue(writer, attr.value);
                    writer.endObject();
                }
                writer.endObject();
            }

            writeSrcLoc(writer, op.srcLoc());
            writer.endObject();
        }
        writer.endArray();

        writer.endObject();
    }

    void Graph::invalidateCaches() const
    {
        valuesCacheDirty_ = true;
        operationsCacheDirty_ = true;
        portsCacheDirty_ = true;
    }

    void Graph::invalidateValuesCache() const
    {
        valuesCacheDirty_ = true;
    }

    void Graph::invalidateOperationsCache() const
    {
        operationsCacheDirty_ = true;
    }

    void Graph::invalidatePortsCache() const
    {
        portsCacheDirty_ = true;
    }

    void Graph::ensureCaches() const
    {
        ensureValuesCache();
        ensureOperationsCache();
        ensurePortsCache();
    }

    void Graph::ensureValuesCache() const
    {
        if (!valuesCacheDirty_)
        {
            return;
        }
        valuesCache_.clear();
        if (builder_)
        {
            const auto &values = builder_->values_;
            valuesCache_.reserve(values.size());
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                if (!values[i].alive)
                {
                    continue;
                }
                ValueId id;
                id.index = static_cast<uint32_t>(i + 1);
                id.generation = 0;
                id.graph = graphId_;
                valuesCache_.push_back(id);
            }
        }
        valuesCacheDirty_ = false;
    }

    void Graph::ensureOperationsCache() const
    {
        if (!operationsCacheDirty_)
        {
            return;
        }
        operationsCache_.clear();
        if (builder_)
        {
            const auto &ops = builder_->operations_;
            operationsCache_.reserve(ops.size());
            for (std::size_t i = 0; i < ops.size(); ++i)
            {
                if (!ops[i].alive)
                {
                    continue;
                }
                OperationId id;
                id.index = static_cast<uint32_t>(i + 1);
                id.generation = 0;
                id.graph = graphId_;
                operationsCache_.push_back(id);
            }
        }
        operationsCacheDirty_ = false;
    }

    void Graph::ensurePortsCache() const
    {
        if (!portsCacheDirty_)
        {
            return;
        }
        inputPortsCache_.clear();
        outputPortsCache_.clear();
        inoutPortsCache_.clear();
        if (builder_)
        {
            inputPortsCache_ = builder_->inputPorts_;
            outputPortsCache_ = builder_->outputPorts_;
            inoutPortsCache_ = builder_->inoutPorts_;
        }
        portsCacheDirty_ = false;
    }

    GraphBuilder &Graph::ensureBuilder()
    {
        if (builder_)
        {
            return *builder_;
        }
        if (view_)
        {
            builder_ = GraphBuilder::fromView(*view_, symbols_);
            view_.reset();
        }
        else
        {
            builder_.emplace(symbols_, graphId_);
        }
        invalidateCaches();
        return *builder_;
    }

    void Graph::reserveSymbolCapacity(std::size_t count)
    {
        symbols_.reserve(count);
        if (builder_)
        {
            builder_->reserveSymbols(count);
        }
    }

    void Graph::reserveDeclaredSymbolCapacity(std::size_t count)
    {
        declaredSymbols_.reserve(count);
        declaredSymbolSet_.reserve(count);
    }

    void Graph::reserveValueCapacity(std::size_t count)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.reserveValues(count);
        if (!valuesCacheDirty_)
        {
            valuesCache_.reserve(count);
        }
    }

    void Graph::reserveOperationCapacity(std::size_t count)
    {
        GraphBuilder &builder = ensureBuilder();
        builder.reserveOperations(count);
        if (!operationsCacheDirty_)
        {
            operationsCache_.reserve(count);
        }
    }

    void Graph::reserveOpOperandCapacity(OperationId op, std::size_t count)
    {
        ensureBuilder().reserveOpOperands(op, count);
    }

    void Graph::reserveOpResultCapacity(OperationId op, std::size_t count)
    {
        ensureBuilder().reserveOpResults(op, count);
    }

    void Graph::reserveOpAttrCapacity(OperationId op, std::size_t count)
    {
        ensureBuilder().reserveOpAttrs(op, count);
    }

    const GraphView &Graph::view() const
    {
        if (!view_)
        {
            throw std::runtime_error("GraphView is not available; freeze the graph first");
        }
        return *view_;
    }

    Value Graph::valueFromView(ValueId id) const
    {
        const GraphView &graphView = view();
        id.assertGraph(graphId_);
        SymbolId symbol = graphView.valueSymbol(id);
        std::string symbolText = symbolTextOrEmpty(symbols_, symbol);
        return Value(id,
                     symbol,
                     std::move(symbolText),
                     graphView.valueWidth(id),
                     graphView.valueSigned(id),
                     graphView.valueType(id),
                     graphView.valueIsInput(id),
                     graphView.valueIsOutput(id),
                     graphView.valueIsInout(id),
                     graphView.valueDef(id),
                     std::span<const ValueUser>(),
                     graphView.valueSrcLoc(id),
                     this,
                     false);
    }

    Value Graph::valueFromBuilder(ValueId id) const
    {
        if (!builder_)
        {
            throw std::runtime_error("GraphBuilder is not available");
        }
        id.assertGraph(graphId_);
        if (id.index == 0 || id.index > builder_->values_.size())
        {
            throw std::runtime_error("ValueId out of range");
        }
        const std::size_t idx = static_cast<std::size_t>(id.index - 1);
        const auto &data = builder_->values_[idx];
        if (!data.alive)
        {
            throw std::runtime_error("ValueId refers to erased value");
        }

        std::string symbolText = symbolTextOrEmpty(symbols_, data.symbol);
        return Value(id,
                     data.symbol,
                     std::move(symbolText),
                     data.width,
                     data.isSigned,
                     data.type,
                     data.isInput,
                     data.isOutput,
                     data.isInout,
                     data.definingOp,
                     std::span<const ValueUser>(),
                     data.srcLoc,
                     this,
                     false);
    }

    Operation Graph::operationFromView(OperationId id) const
    {
        const GraphView &graphView = view();
        id.assertGraph(graphId_);
        SymbolId symbol = graphView.opSymbol(id);
        std::string symbolText = symbolTextOrEmpty(symbols_, symbol);
        return Operation(id,
                         graphView.opKind(id),
                         symbol,
                         std::move(symbolText),
                         std::vector<ValueId>(graphView.opOperands(id).begin(), graphView.opOperands(id).end()),
                         std::vector<ValueId>(graphView.opResults(id).begin(), graphView.opResults(id).end()),
                         std::vector<AttrKV>(graphView.opAttrs(id).begin(), graphView.opAttrs(id).end()),
                         graphView.opSrcLoc(id));
    }

    Operation Graph::operationFromBuilder(OperationId id) const
    {
        if (!builder_)
        {
            throw std::runtime_error("GraphBuilder is not available");
        }
        id.assertGraph(graphId_);
        if (id.index == 0 || id.index > builder_->operations_.size())
        {
            throw std::runtime_error("OperationId out of range");
        }
        const std::size_t idx = static_cast<std::size_t>(id.index - 1);
        const auto &data = builder_->operations_[idx];
        if (!data.alive)
        {
            throw std::runtime_error("OperationId refers to erased operation");
        }

        std::string symbolText = symbolTextOrEmpty(symbols_, data.symbol);
        return Operation(id,
                         data.kind,
                         data.symbol,
                         std::move(symbolText),
                         data.operands,
                         data.results,
                         data.attrs,
                         data.srcLoc);
    }

    std::span<const ValueUser> Graph::valueUsersSpan(ValueId id) const noexcept
    {
        if (builder_)
        {
            if (id.graph != graphId_ || id.index == 0 || id.index > builder_->valueUsers_.size())
            {
                return {};
            }
            const std::size_t idx = static_cast<std::size_t>(id.index - 1);
            if (idx >= builder_->values_.size() || !builder_->values_[idx].alive)
            {
                return {};
            }
            const auto &users = builder_->valueUsers_[idx];
            return std::span<const ValueUser>(users.data(), users.size());
        }
        if (view_)
        {
            return view_->valueUsers(id);
        }
        return {};
    }

    Graph &Design::addGraphInternal(std::unique_ptr<Graph> graph)
    {
        auto sym = graph->symbol();
        auto [it, inserted] = graphs_.emplace(sym, std::move(graph));
        if (!inserted)
        {
            throw std::runtime_error("Duplicated graph symbol: " + sym);
        }
        graphOrder_.push_back(sym);
        return *it->second;
    }

    Graph &Design::createGraph(std::string symbol)
    {
        if (symbol.empty())
        {
            throw std::invalid_argument("Graph symbol must not be empty");
        }
        if (graphs_.contains(symbol))
        {
            throw std::runtime_error("Duplicated graph symbol: " + symbol);
        }
        SymbolId graphSymbol = designSymbols_.contains(symbol) ? designSymbols_.lookup(symbol) : designSymbols_.intern(symbol);
        GraphId graphId = designSymbols_.allocateGraphId(graphSymbol);
        auto instance = std::make_unique<Graph>(*this, std::move(symbol), graphId);
        return addGraphInternal(std::move(instance));
    }

    bool Design::deleteGraph(std::string_view name)
    {
        std::string key(name);
        std::string symbol;
        if (auto it = graphs_.find(key); it != graphs_.end())
        {
            symbol = key;
        }
        else if (auto aliasIt = graphAliasBySymbol_.find(key); aliasIt != graphAliasBySymbol_.end())
        {
            symbol = aliasIt->second;
        }
        else
        {
            return false;
        }

        SymbolId declaredSymbol = designSymbols_.lookup(symbol);
        if (declaredSymbol.valid())
        {
            removeDeclaredSymbol(declaredSymbol);
            designSymbols_.releaseGraphId(declaredSymbol);
        }

        graphs_.erase(symbol);
        auto orderIt = std::remove(graphOrder_.begin(), graphOrder_.end(), symbol);
        if (orderIt != graphOrder_.end())
        {
            graphOrder_.erase(orderIt, graphOrder_.end());
        }

        auto topIt = std::remove(topGraphs_.begin(), topGraphs_.end(), symbol);
        if (topIt != topGraphs_.end())
        {
            topGraphs_.erase(topIt, topGraphs_.end());
        }

        for (auto it = graphAliasBySymbol_.begin(); it != graphAliasBySymbol_.end();)
        {
            if (it->second == symbol)
            {
                it = graphAliasBySymbol_.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return true;
    }

    Graph &Design::cloneGraph(std::string_view sourceName, std::string newName)
    {
        Graph *source = findGraph(sourceName);
        if (!source)
        {
            throw std::runtime_error("Source graph not found: " + std::string(sourceName));
        }
        if (newName.empty())
        {
            throw std::invalid_argument("Graph symbol must not be empty");
        }
        if (graphs_.contains(newName))
        {
            throw std::runtime_error("Duplicated graph symbol: " + newName);
        }

        SymbolId graphSymbol = designSymbols_.contains(newName)
                                   ? designSymbols_.lookup(newName)
                                   : designSymbols_.intern(newName);
        GraphId graphId = designSymbols_.allocateGraphId(graphSymbol);
        auto instance = std::make_unique<Graph>(*this, std::move(newName), graphId);
        Graph &clone = addGraphInternal(std::move(instance));
        cloneGraphContents(*source, clone);

        return clone;
    }

    Design Design::clone() const
    {
        Design cloned;
        for (const auto &name : graphOrder_)
        {
            const Graph *source = findGraph(name);
            if (!source)
            {
                throw std::runtime_error("Graph not found during design clone: " + name);
            }
            Graph &dest = cloned.createGraph(name);
            cloneGraphContents(*source, dest);
        }

        for (const auto &name : graphOrder_)
        {
            Graph *dest = cloned.findGraph(name);
            if (!dest)
            {
                throw std::runtime_error("Graph missing during design clone alias copy: " + name);
            }
            for (const auto &alias : aliasesForGraph(name))
            {
                cloned.registerGraphAlias(alias, *dest);
            }
        }

        for (const auto &name : topGraphs_)
        {
            cloned.markAsTop(name);
        }

        for (const auto sym : declaredSymbols_)
        {
            if (!sym.valid())
            {
                continue;
            }
            std::string_view text = symbolText(sym);
            if (text.empty())
            {
                throw std::runtime_error("Declared symbol text is empty during design clone");
            }
            SymbolId dstSym = cloned.internSymbol(text);
            if (!dstSym.valid())
            {
                throw std::runtime_error("Failed to clone design declared symbol: " + std::string(text));
            }
            cloned.addDeclaredSymbol(dstSym);
        }

        return cloned;
    }

    Graph *Design::findGraph(std::string_view symbol) noexcept
    {
        std::string key(symbol);
        if (auto it = graphs_.find(key); it != graphs_.end())
        {
            return it->second.get();
        }
        if (auto aliasIt = graphAliasBySymbol_.find(key); aliasIt != graphAliasBySymbol_.end())
        {
            if (auto resolved = graphs_.find(aliasIt->second); resolved != graphs_.end())
            {
                return resolved->second.get();
            }
        }
        return nullptr;
    }

    const Graph *Design::findGraph(std::string_view symbol) const noexcept
    {
        std::string key(symbol);
        if (auto it = graphs_.find(key); it != graphs_.end())
        {
            return it->second.get();
        }
        if (auto aliasIt = graphAliasBySymbol_.find(key); aliasIt != graphAliasBySymbol_.end())
        {
            if (auto resolved = graphs_.find(aliasIt->second); resolved != graphs_.end())
            {
                return resolved->second.get();
            }
        }
        return nullptr;
    }

    SymbolId Design::internSymbol(std::string_view text)
    {
        return designSymbols_.intern(text);
    }

    SymbolId Design::lookupSymbol(std::string_view text) const
    {
        return designSymbols_.lookup(text);
    }

    std::string_view Design::symbolText(SymbolId id) const
    {
        if (!id.valid())
        {
            return std::string_view{};
        }
        return designSymbols_.text(id);
    }

    void Design::addDeclaredSymbol(SymbolId sym)
    {
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!designSymbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the design symbol table");
        }
        if (declaredSymbolSet_.insert(sym.value).second)
        {
            declaredSymbols_.push_back(sym);
        }
    }

    bool Design::removeDeclaredSymbol(SymbolId sym)
    {
        if (!sym.valid())
        {
            throw std::runtime_error("Declared symbol is invalid");
        }
        if (!designSymbols_.valid(sym))
        {
            throw std::runtime_error("Declared symbol is not in the design symbol table");
        }
        if (declaredSymbolSet_.erase(sym.value) == 0)
        {
            return false;
        }
        auto it = std::remove_if(declaredSymbols_.begin(), declaredSymbols_.end(),
                                 [&](SymbolId entry) { return entry == sym; });
        if (it != declaredSymbols_.end())
        {
            declaredSymbols_.erase(it, declaredSymbols_.end());
        }
        return true;
    }

    void Design::clearDeclaredSymbols()
    {
        declaredSymbols_.clear();
        declaredSymbolSet_.clear();
    }

    bool Design::isDeclaredSymbol(SymbolId sym) const noexcept
    {
        if (!sym.valid())
        {
            return false;
        }
        return declaredSymbolSet_.find(sym.value) != declaredSymbolSet_.end();
    }

    std::span<const SymbolId> Design::declaredSymbols() const noexcept
    {
        return std::span<const SymbolId>(declaredSymbols_.data(), declaredSymbols_.size());
    }

    std::vector<std::string> Design::aliasesForGraph(std::string_view symbol) const
    {
        std::vector<std::string> aliases;
        for (const auto &entry : graphAliasBySymbol_)
        {
            if (entry.second == symbol)
            {
                aliases.push_back(entry.first);
            }
        }
        std::sort(aliases.begin(), aliases.end());
        aliases.erase(std::unique(aliases.begin(), aliases.end()), aliases.end());
        return aliases;
    }

    void Design::registerGraphAlias(std::string alias, Graph &graph)
    {
        if (alias.empty())
        {
            return;
        }
        graphAliasBySymbol_[std::move(alias)] = graph.symbol();
    }

    void Design::markAsTop(std::string_view graphSymbol)
    {
        if (!findGraph(graphSymbol))
        {
            throw std::runtime_error("Cannot mark unknown graph as top: " + std::string(graphSymbol));
        }
        auto symbolStr = std::string(graphSymbol);
        if (std::find(topGraphs_.begin(), topGraphs_.end(), symbolStr) == topGraphs_.end())
        {
            topGraphs_.push_back(std::move(symbolStr));
        }
    }

    void Design::unmarkAsTop(std::string_view graphSymbol)
    {
        if (!findGraph(graphSymbol))
        {
            throw std::runtime_error("Cannot unmark unknown graph as top: " + std::string(graphSymbol));
        }
        auto symbolStr = std::string(graphSymbol);
        auto it = std::remove(topGraphs_.begin(), topGraphs_.end(), symbolStr);
        if (it != topGraphs_.end())
        {
            topGraphs_.erase(it, topGraphs_.end());
        }
    }

} // namespace wolvrix::lib::grh
