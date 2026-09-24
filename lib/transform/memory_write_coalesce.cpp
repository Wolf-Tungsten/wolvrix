#include "transform/memory_write_coalesce.hpp"

#include "core/grh.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::transform
{
    namespace
    {
        using namespace wolvrix::lib::grh;
        constexpr std::size_t kMaxDepth = 256;
        constexpr std::string_view kPassId = "memory-write-coalesce";

        template <typename T>
        std::optional<T> attr(const Operation &op, std::string_view key)
        {
            const auto value = op.attr(key);
            if (value)
            {
                if (const auto *typed = std::get_if<T>(&*value))
                    return *typed;
            }
            return std::nullopt;
        }

        bool onlyAttrs(const Operation &op, std::initializer_list<std::string_view> allowed)
        {
            for (const auto &item : op.attrs())
                if (std::find(allowed.begin(), allowed.end(), item.key) == allowed.end())
                    return false;
            return true;
        }

        struct Bit
        {
            ValueId value;
            int32_t index = 0;
            char constant = 0;
        };

        struct BitKey
        {
            ValueId value;
            int32_t index;
            bool operator==(const BitKey &) const = default;
        };

        struct BitKeyHash
        {
            std::size_t operator()(const BitKey &key) const
            {
                return ValueIdHash{}(key.value) ^ (static_cast<std::size_t>(key.index) * 0x9e3779b9U);
            }
        };

        // State reads are stable leaves of a sequential RHS. Everything else
        // must be an explicitly recognized combinational operation. In
        // particular, a DPI/instance/system-function result is never a leaf.
        class Analyzer
        {
        public:
            explicit Analyzer(const Graph &graph) : graph_(graph) {}

            bool pure(ValueId value, std::size_t depth = 0)
            {
                if (!value.valid() || depth > kMaxDepth ||
                    graph_.valueType(value) != ValueType::Logic || graph_.valueWidth(value) <= 0)
                    return false;
                if (const auto found = pure_.find(value); found != pure_.end())
                    return found->second;
                if (!visiting_.insert(value).second)
                    return false;
                const auto def = graph_.valueDef(value);
                bool accepted = false;
                if (!def.valid())
                    accepted = graph_.valueIsInput(value) && !graph_.valueIsInout(value);
                else
                {
                    const auto op = graph_.getOperation(def);
                    accepted = pureKind(op);
                    if (accepted && op.kind() != OperationKind::kRegisterReadPort &&
                        op.kind() != OperationKind::kLatchReadPort)
                    {
                        for (const auto operand : op.operands())
                            if (!pure(operand, depth + 1))
                            {
                                accepted = false;
                                break;
                            }
                    }
                }
                visiting_.erase(value);
                pure_.emplace(value, accepted);
                return accepted;
            }

            std::optional<std::vector<char>> constant(ValueId value)
            {
                if (const auto found = constants_.find(value); found != constants_.end())
                    return found->second;
                std::optional<std::vector<char>> result;
                const auto def = graph_.valueDef(value);
                if (def.valid() && graph_.valueType(value) == ValueType::Logic)
                {
                    const auto op = graph_.getOperation(def);
                    const auto literal = attr<std::string>(op, "constValue");
                    const int32_t width = graph_.valueWidth(value);
                    if (op.kind() == OperationKind::kConstant && literal && width > 0 &&
                        onlyAttrs(op, {"constValue"}))
                    {
                        std::string compact;
                        for (char ch : *literal)
                            if (ch != '_' && !std::isspace(static_cast<unsigned char>(ch)))
                                compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
                        if (compact.size() == 2 && compact.front() == '\'' &&
                            std::string_view("01xz").find(compact.back()) != std::string_view::npos)
                            result = std::vector<char>(width, compact.back());
                        else
                        {
                            try
                            {
                                auto parsed = slang::SVInt::fromString(compact);
                                // Avoid inventing a context-sizing interpretation for
                                // malformed / differently sized GRH constants.
                                if (parsed.getBitWidth() == static_cast<slang::bitwidth_t>(width))
                                {
                                    std::vector<char> bits;
                                    bits.reserve(width);
                                    for (int32_t bit = 0; bit < width; ++bit)
                                        bits.push_back(parsed[bit].toChar());
                                    result = std::move(bits);
                                }
                            }
                            catch (const std::exception &) {}
                        }
                    }
                }
                constants_.emplace(value, result);
                return result;
            }

            ValueId assignmentRoot(ValueId value)
            {
                for (std::size_t depth = 0; depth < kMaxDepth; ++depth)
                {
                    const auto def = graph_.valueDef(value);
                    if (!def.valid()) break;
                    const auto op = graph_.getOperation(def);
                    if (op.kind() != OperationKind::kAssign || op.operands().size() != 1 ||
                        !onlyAttrs(op, {}) || graph_.valueWidth(op.operands()[0]) != graph_.valueWidth(value) ||
                        graph_.valueSigned(op.operands()[0]) != graph_.valueSigned(value))
                        break;
                    value = op.operands()[0];
                }
                return value;
            }

            // Trace only routing operations, preserving each original four-state
            // bit. Arithmetic/boolean leaves are kept intact for the data path.
            Bit project(ValueId value, int32_t bit, std::size_t depth = 0)
            {
                const BitKey key{value, bit};
                if (const auto found = projection_.find(key); found != projection_.end())
                    return found->second;
                Bit result{value, bit, 0};
                if (depth >= kMaxDepth || bit < 0 || bit >= graph_.valueWidth(value))
                    return result;
                if (const auto bits = constant(value))
                    result = Bit{{}, 0, (*bits)[bit]};
                else if (const auto def = graph_.valueDef(value); def.valid())
                {
                    const auto op = graph_.getOperation(def);
                    const auto operands = op.operands();
                    const int32_t width = graph_.valueWidth(value);
                    if (op.kind() == OperationKind::kAssign && operands.size() == 1 &&
                        onlyAttrs(op, {}) && graph_.valueWidth(operands[0]) == width)
                        result = project(operands[0], bit, depth + 1);
                    else if (op.kind() == OperationKind::kSliceStatic && operands.size() == 1 &&
                             onlyAttrs(op, {"sliceStart", "sliceEnd"}))
                    {
                        const auto low = attr<int64_t>(op, "sliceStart");
                        const auto high = attr<int64_t>(op, "sliceEnd");
                        if (low && high && *low >= 0 && *high < graph_.valueWidth(operands[0]) &&
                            *high - *low + 1 == width)
                            result = project(operands[0], static_cast<int32_t>(*low + bit), depth + 1);
                    }
                    else if (op.kind() == OperationKind::kReplicate && operands.size() == 1 &&
                             onlyAttrs(op, {"rep"}))
                    {
                        const auto count = attr<int64_t>(op, "rep");
                        const auto sourceWidth = graph_.valueWidth(operands[0]);
                        if (count && *count > 0 && sourceWidth > 0 && *count == width / sourceWidth &&
                            width % sourceWidth == 0)
                            result = project(operands[0], bit % sourceWidth, depth + 1);
                    }
                    else if (op.kind() == OperationKind::kConcat && onlyAttrs(op, {}))
                    {
                        int64_t total = 0;
                        for (const auto operand : operands) total += graph_.valueWidth(operand);
                        if (total == width)
                        {
                            int32_t offset = 0;
                            for (auto it = operands.rbegin(); it != operands.rend(); ++it)
                            {
                                const auto sourceWidth = graph_.valueWidth(*it);
                                if (bit >= offset && bit < offset + sourceWidth)
                                {
                                    result = project(*it, bit - offset, depth + 1);
                                    break;
                                }
                                offset += sourceWidth;
                            }
                        }
                    }
                    else if ((op.kind() == OperationKind::kShl || op.kind() == OperationKind::kLShr) &&
                             operands.size() == 2 && onlyAttrs(op, {}) && !graph_.valueSigned(value) &&
                             !graph_.valueSigned(operands[0]) && !graph_.valueSigned(operands[1]) &&
                             graph_.valueWidth(operands[0]) <= width)
                    {
                        const auto amountBits = constant(operands[1]);
                        if (amountBits)
                        {
                            uint64_t amount = 0;
                            bool known = true;
                            for (std::size_t i = 0; i < amountBits->size(); ++i)
                            {
                                const char digit = (*amountBits)[i];
                                if (digit != '0' && digit != '1') { known = false; break; }
                                if (digit == '1') amount = i >= 63 ? UINT64_MAX : amount | (uint64_t{1} << i);
                            }
                            if (known)
                            {
                                const uint64_t sourceWidth = graph_.valueWidth(operands[0]);
                                if (amount >= static_cast<uint64_t>(width))
                                    result = Bit{{}, 0, '0'};
                                else if (op.kind() == OperationKind::kShl)
                                    result = static_cast<uint64_t>(bit) < amount || static_cast<uint64_t>(bit) - amount >= sourceWidth ?
                                        Bit{{}, 0, '0'} : project(operands[0], static_cast<int32_t>(bit - amount), depth + 1);
                                else
                                    result = static_cast<uint64_t>(bit) + amount >= sourceWidth ?
                                        Bit{{}, 0, '0'} : project(operands[0], static_cast<int32_t>(bit + amount), depth + 1);
                            }
                        }
                    }
                }
                projection_.emplace(key, result);
                return result;
            }

            uint32_t guard(ValueId value, int32_t bit = 0, std::size_t depth = 0)
            {
                const Bit routed = project(value, bit);
                if (routed.constant) return intern(std::string("c:") + routed.constant);
                const BitKey key{routed.value, routed.index};
                if (const auto found = guards_.find(key); found != guards_.end()) return found->second;
                const auto def = graph_.valueDef(routed.value);
                std::string signature = "v:" + std::to_string(routed.value.index) + ":" + std::to_string(routed.index);
                if (depth < kMaxDepth && def.valid())
                {
                    const auto op = graph_.getOperation(def);
                    const auto operands = op.operands();
                    const auto kind = op.kind();
                    const bool unary = kind == OperationKind::kNot || kind == OperationKind::kLogicNot;
                    const bool binary = kind == OperationKind::kAnd || kind == OperationKind::kOr ||
                        kind == OperationKind::kXor || kind == OperationKind::kXnor ||
                        kind == OperationKind::kLogicAnd || kind == OperationKind::kLogicOr;
                    const bool scalarOperands = std::all_of(operands.begin(), operands.end(), [&](ValueId operand) {
                        return graph_.valueWidth(operand) == 1;
                    });
                    if (graph_.valueWidth(routed.value) == 1 && onlyAttrs(op, {}) && scalarOperands &&
                        ((unary && operands.size() == 1) || (binary && operands.size() == 2)))
                    {
                        signature = "op:" + std::to_string(static_cast<int>(kind));
                        for (const auto operand : operands)
                            signature += ":" + std::to_string(guard(operand, 0, depth + 1));
                    }
                }
                const uint32_t result = intern(signature);
                guards_.emplace(key, result);
                return result;
            }

        private:
            static bool pureKind(const Operation &op)
            {
                switch (op.kind())
                {
                case OperationKind::kConstant: return onlyAttrs(op, {"constValue"});
                case OperationKind::kRegisterReadPort:
                    return op.operands().empty() && onlyAttrs(op, {"regSymbol"});
                case OperationKind::kLatchReadPort:
                    return op.operands().empty() && onlyAttrs(op, {"latchSymbol"});
                case OperationKind::kMemoryReadPort: return onlyAttrs(op, {"memSymbol"});
                case OperationKind::kSliceStatic: return onlyAttrs(op, {"sliceStart", "sliceEnd"});
                case OperationKind::kSliceDynamic:
                case OperationKind::kSliceArray: return onlyAttrs(op, {"sliceWidth"});
                case OperationKind::kReplicate: return onlyAttrs(op, {"rep"});
                // Ingest annotates packed-array concatenations for storage
                // backends. These keys describe layout, not side effects. Keep
                // this operation intact; only its purity is relevant here.
                case OperationKind::kConcat:
                    return onlyAttrs(op, {"svPackedArray.version", "svPackedArray.elementWidth",
                        "svPackedArray.elementCount", "svPackedArray.indexLow", "svPackedArray.indexHigh",
                        "svPackedArray.indexDirection", "svPackedArray.laneOrder",
                        "svPackedArray.concat.operand0Index", "svPackedArray.concat.operandStride"});
                case OperationKind::kAdd: case OperationKind::kSub: case OperationKind::kMul:
                case OperationKind::kDiv: case OperationKind::kMod:
                case OperationKind::kEq: case OperationKind::kNe: case OperationKind::kCaseEq:
                case OperationKind::kCaseNe: case OperationKind::kWildcardEq: case OperationKind::kWildcardNe:
                case OperationKind::kLt: case OperationKind::kLe: case OperationKind::kGt: case OperationKind::kGe:
                case OperationKind::kAnd: case OperationKind::kOr: case OperationKind::kXor: case OperationKind::kXnor:
                case OperationKind::kNot: case OperationKind::kLogicAnd: case OperationKind::kLogicOr:
                case OperationKind::kLogicNot: case OperationKind::kReduceAnd: case OperationKind::kReduceOr:
                case OperationKind::kReduceXor: case OperationKind::kReduceNor: case OperationKind::kReduceNand:
                case OperationKind::kReduceXnor: case OperationKind::kShl: case OperationKind::kLShr:
                case OperationKind::kAShr: case OperationKind::kMux: case OperationKind::kAssign:
                    return onlyAttrs(op, {});
                default: return false;
                }
            }

            uint32_t intern(const std::string &signature)
            {
                const auto next = static_cast<uint32_t>(interned_.size() + 1);
                return interned_.try_emplace(signature, next).first->second;
            }

            const Graph &graph_;
            std::unordered_map<ValueId, bool, ValueIdHash> pure_;
            std::unordered_set<ValueId, ValueIdHash> visiting_;
            std::unordered_map<ValueId, std::optional<std::vector<char>>, ValueIdHash> constants_;
            std::unordered_map<BitKey, Bit, BitKeyHash> projection_;
            std::unordered_map<BitKey, uint32_t, BitKeyHash> guards_;
            std::unordered_map<std::string, uint32_t> interned_;
        };

        ValueId makeValue(Graph &graph, OperationKind kind, int32_t width,
                          std::span<const ValueId> operands, std::string_view note)
        {
            const auto value = graph.createValue(graph.makeInternalValSym(), width, false, ValueType::Logic);
            const auto op = graph.createOperation(kind, graph.makeInternalOpSym());
            for (const auto operand : operands) graph.addOperand(op, operand);
            graph.addResult(op, value);
            const auto location = makeTransformSrcLoc(kPassId, note);
            graph.setOpSrcLoc(op, location);
            graph.setValueSrcLoc(value, location);
            return value;
        }

        ValueId makeConstant(Graph &graph, std::span<const char> bits)
        {
            std::string literal = std::to_string(bits.size()) + "'b";
            for (auto it = bits.rbegin(); it != bits.rend(); ++it) literal.push_back(*it);
            const auto value = makeValue(graph, OperationKind::kConstant, static_cast<int32_t>(bits.size()), {}, "mask-or-padding");
            graph.setAttr(graph.valueDef(value), "constValue", std::move(literal));
            return value;
        }

        ValueId materializeData(Graph &graph, const std::vector<Bit> &bits)
        {
            std::vector<ValueId> lowToHigh;
            for (std::size_t low = 0; low < bits.size();)
            {
                std::size_t end = low + 1;
                if (bits[low].constant)
                {
                    while (end < bits.size() && bits[end].constant) ++end;
                    std::vector<char> literal;
                    for (std::size_t bit = low; bit < end; ++bit) literal.push_back(bits[bit].constant);
                    lowToHigh.push_back(makeConstant(graph, literal));
                }
                else
                {
                    while (end < bits.size() && !bits[end].constant && bits[end].value == bits[low].value &&
                           bits[end].index == bits[low].index + static_cast<int32_t>(end - low)) ++end;
                    const auto source = bits[low].value;
                    const auto width = static_cast<int32_t>(end - low);
                    if (bits[low].index == 0 && width == graph.valueWidth(source)) lowToHigh.push_back(source);
                    else
                    {
                        const ValueId operands[] = {source};
                        const auto sliced = makeValue(graph, OperationKind::kSliceStatic, width, operands, "selected-data");
                        const auto sliceOp = graph.valueDef(sliced);
                        graph.setAttr(sliceOp, "sliceStart", static_cast<int64_t>(bits[low].index));
                        graph.setAttr(sliceOp, "sliceEnd", static_cast<int64_t>(bits[low].index + width - 1));
                        lowToHigh.push_back(sliced);
                    }
                }
                low = end;
            }
            if (lowToHigh.size() == 1) return lowToHigh.front();
            std::reverse(lowToHigh.begin(), lowToHigh.end());
            return makeValue(graph, OperationKind::kConcat, static_cast<int32_t>(bits.size()), lowToHigh, "joined-data");
        }

        struct Write
        {
            OperationId op;
            std::vector<ValueId> operands;
            std::vector<char> mask;
        };
    }

    MemoryWriteCoalescePass::MemoryWriteCoalescePass()
        : Pass(std::string(kPassId), std::string(kPassId),
               "Coalesce independent constant-mask memory writes with equivalent guards") {}

    PassResult MemoryWriteCoalescePass::run()
    {
        PassResult result;
        for (const auto &entry : design().graphs())
        {
            Graph &graph = *entry.second;
            Analyzer analyzer(graph);
            std::map<std::string, std::vector<OperationId>> memories;
            std::unordered_set<std::string> otherWriters;
            std::size_t before = 0, removed = 0, groups = 0;
            std::map<std::string, std::size_t> skipped;
            for (const auto opId : graph.operations())
            {
                const auto op = graph.getOperation(opId);
                const auto memory = attr<std::string>(op, "memSymbol");
                if (!memory) continue;
                if (op.kind() == OperationKind::kMemoryWritePort)
                {
                    memories[*memory].push_back(opId);
                    ++before;
                }
                else if (op.kind() != OperationKind::kMemoryReadPort) otherWriters.insert(*memory);
            }

            for (const auto &[memory, operations] : memories)
            {
                if (operations.size() < 2) continue;
                const auto memoryId = graph.findOperation(memory);
                if (!memoryId.valid() || graph.opKind(memoryId) != OperationKind::kMemory)
                { ++skipped["declaration"]; continue; }
                const auto widthAttr = attr<int64_t>(graph.getOperation(memoryId), "width");
                if (!widthAttr || *widthAttr <= 0 || *widthAttr > INT32_MAX)
                { ++skipped["width"]; continue; }
                const int32_t width = static_cast<int32_t>(*widthAttr);
                if (otherWriters.contains(memory)) { ++skipped["other_writer"]; continue; }
                std::vector<uint8_t> occupied(width, 0);
                std::vector<Write> writes;
                std::string rejection;
                for (const auto opId : operations)
                {
                    const auto op = graph.getOperation(opId);
                    const auto operands = op.operands();
                    if (op.attr(kMemoryWritePriorityAttr) || op.attr(kMemoryWritePriorityGroupAttr))
                    { rejection = "priority"; break; }
                    if (!onlyAttrs(op, {"memSymbol", "eventEdge"})) { rejection = "attributes"; break; }
                    const auto edges = attr<std::vector<std::string>>(op, "eventEdge");
                    if (operands.size() < 5 || !edges || edges->size() != operands.size() - 4 ||
                        std::any_of(operands.begin(), operands.end(), [](ValueId value) { return !value.valid(); }) ||
                        !op.results().empty() || graph.valueWidth(operands[0]) != 1 ||
                        graph.valueWidth(operands[2]) != width || graph.valueWidth(operands[3]) != width)
                    { rejection = "shape"; break; }
                    const auto mask = analyzer.constant(operands[3]);
                    if (!mask || std::any_of(mask->begin(), mask->end(), [](char bit) { return bit != '0' && bit != '1'; }))
                    { rejection = "mask"; break; }
                    for (int32_t bit = 0; bit < width; ++bit)
                        if ((*mask)[bit] == '1')
                        {
                            if (occupied[bit]) { rejection = "overlap"; break; }
                            occupied[bit] = 1;
                        }
                    if (!rejection.empty()) break;
                    if (!std::all_of(operands.begin(), operands.end(), [&](ValueId value) { return analyzer.pure(value); }))
                    { rejection = "dependency"; break; }
                    writes.push_back(Write{opId, {operands.begin(), operands.end()}, *mask});
                }
                if (!rejection.empty()) { ++skipped[rejection]; continue; }

                std::map<std::string, std::vector<std::size_t>> byKey;
                for (std::size_t index = 0; index < writes.size(); ++index)
                {
                    const auto &write = writes[index];
                    const auto op = graph.getOperation(write.op);
                    if (keepDeclaredSymbols() && graph.isDeclaredSymbol(op.symbol()))
                    { ++skipped["declared_port"]; continue; }
                    const auto edges = *attr<std::vector<std::string>>(op, "eventEdge");
                    std::string key = std::to_string(analyzer.guard(write.operands[0])) + ":a" +
                        std::to_string(analyzer.assignmentRoot(write.operands[1]).index);
                    for (std::size_t event = 0; event < edges.size(); ++event)
                        key += ":e" + std::to_string(write.operands[4 + event].index) + ":" + edges[event];
                    byKey[key].push_back(index);
                }
                for (const auto &[key, members] : byKey)
                {
                    (void)key;
                    if (members.size() < 2) continue;
                    std::vector<Bit> data(width, Bit{{}, 0, '0'});
                    std::vector<char> mask(width, '0');
                    for (const auto member : members)
                        for (int32_t bit = 0; bit < width; ++bit)
                            if (writes[member].mask[bit] == '1')
                            {
                                mask[bit] = '1';
                                data[bit] = analyzer.project(writes[member].operands[2], bit);
                            }
                    const auto newData = materializeData(graph, data);
                    const auto newMask = makeConstant(graph, mask);
                    const auto anchor = writes[members.front()].op;
                    graph.replaceOperand(anchor, 2, newData);
                    graph.replaceOperand(anchor, 3, newMask);
                    for (std::size_t index = 1; index < members.size(); ++index)
                        if (!graph.eraseOp(writes[members[index]].op))
                        {
                            error(graph, "failed to erase a coalesced memory write");
                            result.failed = true;
                            return result;
                        }
                    ++groups;
                    removed += members.size() - 1;
                    result.changed = true;
                }
            }
            std::string summary = "memory-write-coalesce ports_before=" + std::to_string(before) +
                " ports_after=" + std::to_string(before - removed) + " groups=" + std::to_string(groups) +
                " removed=" + std::to_string(removed);
            for (const auto &[reason, count] : skipped) summary += " skipped_" + reason + "=" + std::to_string(count);
            info(graph, summary);
        }
        return result;
    }
}
