#include "transform/grhsim/ops.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace wolvrix::lib::transform::grhsim {
namespace {
    using namespace grh;
    constexpr std::size_t any = std::numeric_limits<std::size_t>::max();
    template <typename T> constexpr std::size_t index = AttributeValue(T{}).index();
    using Owner = std::vector<std::string>;

    const Owner* ownerAttribute(const Graph& graph, OperationId op)
    {
        for (const auto& entry : graph.opAttrs(op))
            if (entry.key == kPartition) {
                if (const auto* owner = std::get_if<Owner>(&entry.value)) return owner;
                throw std::invalid_argument("Wrong attribute type: " + std::string(kPartition));
            }
        return nullptr;
    }

    std::string partitionTopology(const Graph& graph)
    {
        const auto operations = graph.operations();
        uint32_t last = 0;
        for (const auto op : operations) last = std::max(last, op.index);
        std::vector<std::size_t> positions(static_cast<std::size_t>(last) + 1);
        const Owner empty;
        std::vector<const Owner*> owners;
        owners.reserve(operations.size());
        std::map<int64_t, std::size_t> effects;
        for (const auto op : graph.operations()) {
            positions[op.index] = owners.size();
            const auto* owner = ownerAttribute(graph, op);
            owners.push_back(owner ? owner : &empty);
            if (const auto order = attribute<int64_t>(graph, op, "grhsim.effectOrder")) effects.emplace(*order, positions[op.index]);
        }
        std::vector<std::pair<std::size_t, std::size_t>> edges;
        for (const auto op : graph.operations()) {
            const auto target = positions[op.index];
            if (owners[target]->empty()) continue;
            for (const auto input : graph.opOperands(op)) {
                const auto producer = graph.valueDef(input);
                if (!producer) continue;
                const auto source = positions[producer.index];
                // Cross-phase pulses and held samples are protocol boundaries,
                // not same-round combinational dependencies.
                if (!owners[source]->empty() && (*owners[source])[0] == (*owners[target])[0]) edges.emplace_back(source, target);
            }
        }
        std::optional<std::size_t> previous;
        for (const auto& [order, current] : effects) {
            if (previous) edges.emplace_back(*previous, current);
            previous = current;
        }
        const auto acyclic = [](std::size_t count, const auto& links) {
            // Count repeated operand edges on both sides rather than
            // allocating a tree node for every distinct graph edge.
            std::vector<std::size_t> offsets(count + 1), degree(count), ready;
            ready.reserve(count);
            for (const auto& [source, target] : links) { ++offsets[source + 1]; ++degree[target]; }
            for (std::size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i - 1];
            auto cursor = offsets;
            std::vector<std::size_t> successors(links.size());
            for (const auto& [source, target] : links) successors[cursor[source]++] = target;
            for (std::size_t i = 0; i < count; ++i) if (!degree[i]) ready.push_back(i);
            for (std::size_t i = 0; i < ready.size(); ++i)
                for (auto edge = offsets[ready[i]]; edge < offsets[ready[i] + 1]; ++edge)
                    if (--degree[successors[edge]] == 0) ready.push_back(successors[edge]);
            return ready.size() == count;
        };
        if (!acyclic(owners.size(), edges)) return "Cyclic computation or effect-order dependencies";
        for (const bool supernodes : {false, true}) {
            std::map<Owner, std::size_t> indices;
            std::vector<std::size_t> quotient(owners.size());
            for (std::size_t i = 0; i < owners.size(); ++i) {
                auto key = *owners[i];
                if (supernodes && key.size() == 4 && key[1] == "nonsink") key.resize(3);
                quotient[i] = indices.emplace(std::move(key), indices.size()).first->second;
            }
            std::vector<std::pair<std::size_t, std::size_t>> links;
            for (const auto& [source, target] : edges)
                if (quotient[source] != quotient[target]) links.emplace_back(quotient[source], quotient[target]);
            if (!acyclic(indices.size(), links)) return "Cyclic virtual partition dependencies";
        }
        return {};
    }

    bool bit(const Graph& graph, ValueId value, bool pulse = false)
    {
        const LogicalType type = graph.valueLogicalType(value);
        return type.kind == ValueType::Logic && type.width == 1 && type.shape.empty() &&
               (!pulse || (!type.isSigned && type.domain == LogicDomain::TwoState));
    }

    bool scalarTwoState(const LogicalType& type)
    {
        return type.kind == ValueType::Logic && type.domain == LogicDomain::TwoState &&
               type.shape.empty() && type.width > 0 && type.width <= 64;
    }

    std::string edgeVerifier(const Graph& graph, OperationId op)
    {
        if (!bit(graph, graph.opOperands(op)[0]) || !bit(graph, graph.opResults(op)[0], true))
            return "edge-detect requires one scalar Logic bit and returns an unsigned TwoState bit";
        const auto edge = attribute<std::string>(graph, op, "edge").value();
        if (edge != "posedge" && edge != "negedge" && edge != "both") return "Unknown edge mode";
        const auto initial = slang::SVInt::fromString(attribute<std::string>(graph, op, "prevInit").value());
        if (initial.hasUnknown() && graph.valueDomain(graph.opOperands(op)[0]) == LogicDomain::TwoState)
            return "TwoState event history cannot contain X/Z";
        if (initial.getBitWidth() != 1) return "Event history must be a one-bit literal";
        if (attribute<std::string>(graph, op, "sampleScope")->empty() ||
            attribute<std::string>(graph, op, "historyUpdate").value() != "unconditional")
            return "edge-detect requires a sampling scope and unconditional per-round history update";
        return {};
    }

    std::string writerVerifier(const Graph& graph, OperationId op)
    {
        const OperationKind kind = graph.opKind(op);
        const auto operands = graph.opOperands(op);
        const bool latch = kind == kLatchWrite;
        if (!latch && !isFire(graph, operands[0])) return "Clocked write fire is not an edge pulse or legal OR";
        if (!bit(graph, operands[latch ? 0 : 1])) return "Write enable must be one scalar Logic bit";
        const auto order = attribute<int64_t>(graph, op, "writerOrder");
        if (!order || *order < 0) return "Write requires nonnegative writerOrder";
        const auto target = writerTarget(graph, op);
        const LogicalType type = stateLogicalType(graph, target);
        if ((graph.opKind(target) == OperationKind::kLatch) != latch)
            return "Writer references the wrong declaration kind";
        const bool masked = kind == kWriteMask || kind == kWriteArrayMask || latch;
        const bool indexed = kind == kWriteArray || kind == kWriteArrayMask;
        int32_t width = type.width;
        if (indexed)
        {
            const std::size_t count = operands.size() - (masked ? 4 : 3);
            if (count < 1 || count > type.shape.size()) return "Invalid indexed write rank";
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto indexType = graph.valueLogicalType(operands[2 + i]);
                if (indexType.kind != ValueType::Logic || !indexType.shape.empty())
                    return "Array indices must be scalar Logic values";
                width /= static_cast<int32_t>(type.shape[i]);
            }
        }
        const auto data = operands[operands.size() - (masked ? 2 : 1)];
        const LogicalType dataType = graph.valueLogicalType(data);
        if (dataType.kind != type.kind || dataType.domain != type.domain)
            return "Write data kind/domain differs from the declaration";
        if (kind == kFill)
        {
            if (type.shape.empty() || (dataType.width != type.leafWidth() && dataType.width != type.width))
                return "fill requires an array and element-width or full-width data";
        }
        else if (dataType.width != width) return "Write data width does not match the addressed state";
        if (masked)
        {
            const LogicalType mask = graph.valueLogicalType(operands.back());
            if (mask.kind != ValueType::Logic || mask.width != width || mask.domain != type.domain)
                return "Write mask must match the addressed width/domain";
        }
        return {};
    }

    std::string taskVerifier(const Graph& graph, OperationId op)
    {
        const auto operands = graph.opOperands(op);
        const bool eventDriven = attribute<bool>(graph, op, "eventDriven").value();
        if (!isFire(graph, operands[0], !eventDriven) || !bit(graph, operands[1])) return "Invalid task fire/callCond";
        if (attribute<std::string>(graph, op, "name")->empty()) return "Task name is empty";
        if (attribute<std::string>(graph, op, "procKind")->empty()) return "Task process kind is empty";
        return {};
    }

    std::string historyVerifier(const Graph& graph, OperationId op)
    {
        const auto state = writerTarget(graph, op);
        if (graph.opKind(state) != OperationKind::kRegister ||
            attribute<std::string>(graph, state, "grhsim.history") != "output-end" ||
            !attribute<bool>(graph, op, "grhsim.outputEnd").value_or(false))
            return "history-write requires an output-end history register";
        if (stateLogicalType(graph, state) != graph.valueLogicalType(graph.opOperands(op)[0]))
            return "history-write must preserve the complete history type";
        return {};
    }

    OpEffect functionEffect(const Graph& graph, OperationId op)
    {
        return systemFunctionEffect(graph, op) == "pure" ? OpEffect::Pure : OpEffect::External;
    }

    std::string functionVerifier(const Graph& graph, OperationId op)
    {
        const auto name = attribute<std::string>(graph, op, "name").value();
        if (name.empty()) return "System function name is empty";
        const auto effect = attribute<std::string>(graph, op, "effect").value();
        const auto expected = systemFunctionEffect(graph, op);
        if (effect != expected) return "System function effect does not match its name";
        if (expected != "pure")
        {
            if (attribute<std::string>(graph, op, "sampleId").value_or("").empty() ||
                attribute<std::string>(graph, op, "sampleScope").value_or("").empty())
                return "Non-pure function requires sample identity and scope";
        }
        return {};
    }

    std::string dpiVerifier(const Graph& graph, OperationId op)
    {
        const auto operands = graph.opOperands(op);
        const bool eventDriven = attribute<bool>(graph, op, "eventDriven").value();
        if (!isFire(graph, operands[0], !eventDriven) || !bit(graph, operands[1])) return "Invalid DPI fire/callCond";
        if (attribute<std::string>(graph, op, "procKind")->empty()) return "DPI process kind is empty";
        return verifyDpiSignature(graph, op, 2);
    }

    std::string dpiSignature(const Graph& graph, OperationId op, std::size_t firstArgument,
                             std::size_t trailingEvents, bool permitInputSignViews)
    {
        const auto operands = graph.opOperands(op);
        if (trailingEvents > operands.size() || firstArgument > operands.size() - trailingEvents)
            return "DPI argument range is invalid";
        const std::size_t inputEnd = operands.size() - trailingEvents;
        const auto targetName = attribute<std::string>(graph, op, "targetImportSymbol").value();
        const auto target = graph.findOperation(targetName);
        if (!target || graph.opKind(target) != OperationKind::kDpicImport) return "DPI import does not resolve";
        const auto directions = attribute<std::vector<std::string>>(graph, target, "argsDirection").value_or(std::vector<std::string>{});
        const auto widths = attribute<std::vector<int64_t>>(graph, target, "argsWidth").value_or(std::vector<int64_t>{});
        const auto signedness = attribute<std::vector<bool>>(graph, target, "argsSigned").value_or(std::vector<bool>{});
        const auto names = attribute<std::vector<std::string>>(graph, target, "argsName").value_or(std::vector<std::string>{});
        const auto kinds = attribute<std::vector<std::string>>(graph, target, "argsType").value_or(std::vector<std::string>{});
        if (widths.size() != directions.size() || signedness.size() != directions.size() || names.size() != directions.size() ||
            (!kinds.empty() && kinds.size() != directions.size())) return "DPI metadata lengths differ";
        std::size_t input = firstArgument;
        const bool hasReturn = attribute<bool>(graph, target, "hasReturn").value_or(false);
        if (const auto callReturn = attribute<bool>(graph, op, "hasReturn"); callReturn && *callReturn != hasReturn)
            return "DPI call/import hasReturn differs";
        std::size_t output = hasReturn ? 1 : 0;
        const auto results = graph.opResults(op);
        if (output && results.empty()) return "DPI return result is missing";
        const auto matches = [&](ValueId value, std::string_view kind, int64_t width, bool sign, bool input = false) {
            const auto parsed = parseDpiValueType(kind);
            const auto actual = graph.valueLogicalType(value);
            return parsed && actual.kind == *parsed && actual.shape.empty() &&
                   (*parsed != ValueType::Logic || (actual.width == width &&
                    (actual.isSigned == sign || (input && permitInputSignViews))));
        };
        if (output && !matches(results[0], attribute<std::string>(graph, target, "returnType").value_or("logic"),
                              attribute<int64_t>(graph, target, "returnWidth").value_or(1),
                              attribute<bool>(graph, target, "returnSigned").value_or(false)))
            return "DPI return type differs from the import";
        for (std::size_t i = 0; i < directions.size(); ++i)
            if (directions[i] != "input" && directions[i] != "output" && directions[i] != "inout")
                return "Unknown DPI argument direction";
        for (std::size_t pass = 0; pass < 4; ++pass)
        {
            // GRH groups inputs before inouts, and outputs before inout results.
            const std::string_view direction = pass == 0 ? "input" : pass == 2 ? "output" : "inout";
            const bool consumes = pass < 2;
            for (std::size_t i = 0; i < directions.size(); ++i)
            {
                if (directions[i] != direction) continue;
                const auto kind = kinds.empty() ? "logic" : kinds[i];
                if (consumes)
                {
                    if (input >= inputEnd || !matches(operands[input++], kind, widths[i], signedness[i], true))
                        return "DPI input/inout type or count differs from the import";
                }
                else if (output >= results.size() || !matches(results[output++], kind, widths[i], signedness[i]))
                    return "DPI output/inout type or count differs from the import";
            }
        }
        if (input != inputEnd || output != results.size()) return "DPI operand/result count differs from the import";
        return {};
    }

    std::string bitSelectVerifier(const Graph& graph, OperationId op)
    {
        const auto result = graph.opResults(op)[0];
        if (!scalarTwoState(graph.valueLogicalType(result))) return "bitSelect requires scalar TwoState width <=64";
        for (const ValueId operand : graph.opOperands(op))
            if (!graph.sameValueType(result, operand)) return "bitSelect operands and result must have identical full types";
        return {};
    }

    std::string priorityVerifier(const Graph& graph, OperationId op)
    {
        const auto operands = graph.opOperands(op);
        if (operands.size() % 2 == 0) return "prioritySelect requires 2N+1 operands";
        const auto count = (operands.size() - 1) / 2;
        if (!scalarTwoState(graph.valueLogicalType(graph.opResults(op)[0]))) return "Invalid prioritySelect result";
        for (std::size_t i = 0; i < operands.size(); ++i)
            if (i < count ? !bit(graph, operands[i], true) : !scalarTwoState(graph.valueLogicalType(operands[i])))
                return "Invalid prioritySelect condition or arm type";
        return {};
    }

    std::string maskedOrVerifier(const Graph& graph, OperationId op)
    {
        const LogicalType result = graph.valueLogicalType(graph.opResults(op)[0]);
        if (result.kind != ValueType::Logic || result.domain != LogicDomain::TwoState) return "maskedOr requires TwoState Logic";
        const auto operands = graph.opOperands(op);
        for (const ValueId operand : operands)
        {
            const LogicalType type = graph.valueLogicalType(operand);
            if (type.kind != ValueType::Logic || type.domain != LogicDomain::TwoState) return "maskedOr operand is not TwoState Logic";
        }
        const auto views = attribute<std::vector<int64_t>>(graph, op, "views");
        if (!views)
        {
            if (operands.size() != 2) return "Dense maskedOr requires mask and array";
            const auto mask = graph.valueLogicalType(operands[0]);
            const auto data = graph.valueLogicalType(operands[1]);
            if (!mask.shape.empty() || mask.width < 2 || data.shape != std::vector<int64_t>{mask.width} ||
                data.width != static_cast<int64_t>(mask.width) * result.width) return "Invalid dense maskedOr geometry";
        }
        else
        {
            if (views->size() < 8 || views->size() % 4 != 0) return "maskedOr views require N>=2 four-field tuples";
            for (std::size_t i = 0; i < views->size(); i += 4)
                for (const auto field : {0u, 2u})
                {
                    const auto slot = (*views)[i + field];
                    const auto offset = (*views)[i + field + 1];
                    if (slot < 0 || static_cast<uint64_t>(slot) >= operands.size() || offset < 0 ||
                        offset > graph.valueWidth(operands[slot]) - (field == 0 ? 1 : result.width))
                        return "maskedOr view is outside its operand";
                }
        }
        return {};
    }

    constexpr AttrSchema edgeAttrs[]{{"edge", index<std::string>, true}, {"prevInit", index<std::string>, true},
        {"sampleScope", index<std::string>, true}, {"historyUpdate", index<std::string>, true}};
    constexpr AttrSchema writeAttrs[]{{"regSymbol", index<std::string>, true}, {"writerOrder", index<int64_t>, true}};
    constexpr AttrSchema latchAttrs[]{{"latchSymbol", index<std::string>, true}, {"writerOrder", index<int64_t>, true}};
    constexpr AttrSchema taskAttrs[]{{"name", index<std::string>, true}, {"procKind", index<std::string>, true},
        {"hasTiming", index<bool>, true}, {"eventDriven", index<bool>, true}};
    constexpr AttrSchema functionAttrs[]{{"name", index<std::string>, true}, {"effect", index<std::string>, true},
        {"sampleId", index<std::string>}, {"sampleScope", index<std::string>}, {"hasSideEffects", index<bool>}};
    constexpr AttrSchema dpiAttrs[]{{"targetImportSymbol", index<std::string>, true}, {"procKind", index<std::string>, true},
        {"hasTiming", index<bool>, true}, {"eventDriven", index<bool>, true}};
    constexpr AttrSchema maskedAttrs[]{{"views", index<std::vector<int64_t>>}};
    constexpr AttrSchema historyAttrs[]{{"regSymbol", index<std::string>, true}, {"grhsim.outputEnd", index<bool>, true}};
    const OpDescriptor descriptors[]{
        {kEdgeDetect, "grhsim.edge-detect", "grhsim", 1, OpEffect::History, false, false, 1, 1, 1, 1, edgeAttrs, edgeVerifier},
        {kWrite, "grhsim.write", "grhsim", 1, OpEffect::WriteState, false, false, 3, 3, 0, 0, writeAttrs, writerVerifier},
        {kWriteMask, "grhsim.writeMask", "grhsim", 1, OpEffect::WriteState, false, false, 4, 4, 0, 0, writeAttrs, writerVerifier},
        {kWriteArray, "grhsim.writeArray", "grhsim", 1, OpEffect::WriteState, false, false, 4, any, 0, 0, writeAttrs, writerVerifier},
        {kWriteArrayMask, "grhsim.writeArrayMask", "grhsim", 1, OpEffect::WriteState, false, false, 5, any, 0, 0, writeAttrs, writerVerifier},
        {kFill, "grhsim.fill", "grhsim", 1, OpEffect::WriteState, false, false, 3, 3, 0, 0, writeAttrs, writerVerifier},
        {kLatchWrite, "grhsim.latchWrite", "grhsim", 1, OpEffect::WriteState, false, false, 3, 3, 0, 0, latchAttrs, writerVerifier},
        {kSystemTask, "grhsim.system-task", "grhsim", 1, OpEffect::External, false, false, 2, any, 0, 0, taskAttrs, taskVerifier},
        {kSystemFunction, "grhsim.system-function", "grhsim", 1, OpEffect::External, true, true, 0, any, 1, 1, functionAttrs, functionVerifier, functionEffect},
        {kDpiCall, "grhsim.dpi-call", "grhsim", 1, OpEffect::External, false, false, 2, any, 0, any, dpiAttrs, dpiVerifier},
        {kBitSelect, "grhsim.bitSelect", "grhsim", 1, OpEffect::Pure, true, true, 3, 3, 1, 1, {}, bitSelectVerifier},
        {kPrioritySelect, "grhsim.prioritySelect", "grhsim", 1, OpEffect::Pure, true, true, 7, 129, 1, 1, {}, priorityVerifier},
        {kHistoryWrite, "grhsim.history-write", "grhsim", 1, OpEffect::History, false, false, 1, 1, 0, 0, historyAttrs, historyVerifier},
        {kMaskedOr, "grhsim.cpu.maskedOr", "grhsim.cpu", 1, OpEffect::Pure, true, true, 1, any, 1, 1, maskedAttrs, maskedOrVerifier}
    };
    const bool registered = [] { registerOpDescriptors(descriptors); return true; }();
}

bool isWriter(grh::OperationKind kind) noexcept
{
    return kind == kWrite || kind == kWriteMask || kind == kWriteArray ||
           kind == kWriteArrayMask || kind == kFill || kind == kLatchWrite;
}

bool isPureSystemFunction(std::string_view name) noexcept
{
    constexpr std::string_view names[]{"clog2", "bits", "countbits", "countones", "isunknown", "onehot", "onehot0",
        "signed", "unsigned", "itor", "rtoi", "bitstoreal", "realtobits", "shortrealtobits", "bitstoshortreal",
        "ln", "log10", "exp", "sqrt", "pow", "floor", "ceil", "sin", "cos", "tan", "asin", "acos", "atan",
        "atan2", "hypot", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh"};
    return std::ranges::find(names, name) != std::end(names);
}

bool isSampledSystemFunction(std::string_view name) noexcept
{
    return name == "random" || name == "urandom" || name == "urandom_range" || name == "time" || name == "realtime";
}

std::string_view systemFunctionEffect(const grh::Graph& graph, grh::OperationId operation)
{
    const auto name = attribute<std::string>(graph, operation, "name").value_or("");
    const bool hasSideEffects = attribute<bool>(graph, operation, "hasSideEffects").value_or(false);
    if (isPureSystemFunction(name) && !hasSideEffects) return "pure";
    return isSampledSystemFunction(name) ? "sample" : "external";
}

bool isFire(const grh::Graph& graph, grh::ValueId value, bool allowAlways)
{
    using namespace grh;
    std::unordered_set<ValueId, ValueIdHash> visiting;
    std::unordered_map<ValueId, bool, ValueIdHash> memo;
    std::function<bool(ValueId)> visit = [&](ValueId current) {
        if (!bit(graph, current, true)) return false;
        if (const auto known = memo.find(current); known != memo.end()) return known->second;
        if (!visiting.insert(current).second) return false;
        const OperationId def = graph.valueDef(current);
        bool valid = false;
        if (def)
        {
            const OperationKind kind = graph.opKind(def);
            const auto args = graph.opOperands(def);
            if (kind == kEdgeDetect) valid = true;
            else if (kind == OperationKind::kConstant)
            {
                const auto literal = attribute<std::string>(graph, def, "constValue");
                if (literal)
                {
                    const auto bits = slang::SVInt::fromString(*literal).resize(1);
                    valid = !bits.hasUnknown() && (allowAlways || bits.getRawPtr()[0] == 0);
                }
            }
            else if (kind == OperationKind::kAssign && args.size() == 1) valid = visit(args[0]);
            else if (kind == OperationKind::kOr && args.size() == 2) valid = visit(args[0]) && visit(args[1]);
        }
        visiting.erase(current);
        memo.emplace(current, valid);
        return valid;
    };
    return visit(value);
}

grh::OperationId writerTarget(const grh::Graph& graph, grh::OperationId writer)
{
    const auto key = graph.opKind(writer) == kLatchWrite ? "latchSymbol" : "regSymbol";
    const auto name = attribute<std::string>(graph, writer, key);
    if (!name) throw std::invalid_argument("Writer has no target symbol");
    const auto target = graph.findOperation(*name);
    if (!target) throw std::invalid_argument("Writer target does not resolve: " + *name);
    return target;
}

std::optional<grh::ValueType> parseDpiValueType(std::string_view name) noexcept
{
    if (name == "bit" || name == "byte" || name == "shortint" || name == "int" ||
        name == "longint" || name == "integer" || name == "time") return grh::ValueType::Logic;
    if (name == "shortreal" || name == "realtime") return grh::ValueType::Real;
    return grh::parseValueType(name);
}

std::string verifyDpiSignature(const grh::Graph& graph, grh::OperationId call,
                               std::size_t firstArgument, std::size_t trailingEvents, bool permitInputSignViews)
{
    return dpiSignature(graph, call, firstArgument, trailingEvents, permitInputSignViews);
}

std::string verifyGraph(const grh::Graph& graph)
{
    using namespace grh;
    const auto stageAttribute = graph.metadata(kStage);
    if (!stageAttribute) return {};
    const auto* stage = std::get_if<std::string>(&*stageAttribute);
    if (!stage) return "grhsim.stage must be a string";
    const std::set<std::string> stages{"converted", "event-output-split", "nonsink-sink-split", "nonsink-partitioned", "sink-partitioned"};
    if (!stages.contains(*stage)) return "Unknown GrhSIM graph stage";
    std::set<Owner> leaves;
    std::map<OperationId, uint32_t, std::function<bool(OperationId, OperationId)>> historyWriters(
        [](OperationId a, OperationId b) { return a.index < b.index; });
    std::set<int64_t> effectOrders;
    const Owner emptyOwner;
    const auto pathOf = [&](OperationId op) -> const Owner& {
        const auto* owner = ownerAttribute(graph, op);
        return owner ? *owner : emptyOwner;
    };
    const auto has = [&](OperationId op, std::string_view key) {
        return std::any_of(graph.opAttrs(op).begin(), graph.opAttrs(op).end(),
                           [&](const auto& entry) { return entry.key == key; });
    };
    for (const OperationId op : graph.operations())
    {
        const OperationKind kind = graph.opKind(op);
        const bool declaration = kind == OperationKind::kRegister || kind == OperationKind::kLatch || kind == OperationKind::kDpicImport;
        if (kind == OperationKind::kRegister || kind == OperationKind::kLatch)
        {
            if (!has(op, "valueType") || !has(op, "domain") || !has(op, "initSpecKinds"))
                return "Converted state requires explicit type/domain and typed InitSpec";
            stateLogicalType(graph, op);
            stateInitSpec(graph, op);
        }
        if (isRegisterWritePortKind(kind) || kind == OperationKind::kLatchWritePort ||
            kind == OperationKind::kSystemTask || kind == OperationKind::kSystemFunction || kind == OperationKind::kDpicCall ||
            kind == OperationKind::kInstance || kind == OperationKind::kXMRRead || kind == OperationKind::kXMRWrite || kind == OperationKind::kBlackbox)
            return "Incomplete conversion or non-flat GrhSIM graph";
        const auto* owner = ownerAttribute(graph, op);
        if (*stage == "converted" && owner) return "Converted graph must not contain virtual partitions";
        if (*stage == "converted" && (kind == kHistoryWrite || has(op, "grhsim.history") ||
            has(op, "grhsim.timeslot") || has(op, "grhsim.sampleBoundary")))
            return "Converted graph must not contain extraction histories or boundaries";
        if (*stage != "converted" && !declaration && (!owner || owner->empty())) return "Partitioned graph has an executable op without an owner";
        if (owner)
        {
            if (declaration) return "Shared declarations must not have a virtual partition";
            if (owner->empty() || ((*owner)[0] != "general" && (*owner)[0] != "event" && (*owner)[0] != "output"))
                return "Unknown virtual partition root";
            if (*stage == "event-output-split" && owner->size() != 1) return "Premature partition refinement";
            if (((*owner)[0] == "event" || (*owner)[0] == "output") && owner->size() != 1)
                return "Event and Output are not refined by General partition passes";
            if (*stage == "nonsink-sink-split" && (*owner)[0] == "general" && owner->size() != 2)
                return "Premature General partition refinement";
            if (*stage == "nonsink-partitioned" && (*owner)[0] == "general" &&
                owner->size() > 2 && (*owner)[1] != "nonsink")
                return "Premature sink partition refinement";
            if (*stage != "event-output-split" && (*owner)[0] == "general" &&
                (owner->size() < 2 || ((*owner)[1] != "nonsink" && (*owner)[1] != "small-sink" && (*owner)[1] != "large-sink")))
                return "Invalid General partition path";
            for (const auto& component : *owner) if (component.empty()) return "Empty virtual partition key";
            leaves.insert(*owner);
            if (*stage == "nonsink-partitioned" || *stage == "sink-partitioned")
                if ((*owner)[0] == "general" && (*owner)[1] == "nonsink" && owner->size() != 4)
                    return "Nonsink requires supernode and node ownership";
            if (*stage == "sink-partitioned" && (*owner)[0] == "general" && (*owner)[1] != "nonsink" && owner->size() != 3)
                return "Sink requires complete group ownership";
            if (kind == kEdgeDetect && *owner != Owner{"event"}) return "edge-detect must belong to Event";
            if (kind == kHistoryWrite) {
                if (*owner != Owner{"output"}) return "History writes must belong to Output";
                ++historyWriters[writerTarget(graph, op)];
            }
            if (kind == kSystemTask) {
                const auto name = attribute<std::string>(graph, op, "name").value();
                const auto timeslot = attribute<std::string>(graph, op, "grhsim.timeslot");
                if (name == "monitor" || name == "strobe") {
                    if (*owner != Owner{"output"} || !timeslot ||
                        *timeslot != (attribute<bool>(graph, op, "eventDriven").value() ? "event" : "change"))
                        return "Timeslot task requires Output and explicit event/change semantics";
                } else if ((*owner)[0] != "general") return "Ordinary task must belong to General";
            }
            if (kind == kDpiCall || (kind == kSystemFunction && operationEffect(graph, op) != OpEffect::Pure))
                if ((*owner)[0] != "general") return "Effectful samples must execute in General";
            if (isWriter(kind)) {
                if ((*owner)[0] != "general") return "Ordinary writer must belong to General";
                if (attribute<std::string>(graph, writerTarget(graph, op), "grhsim.history") == "output-end")
                    return "Output history cannot have an ordinary writer";
                if (*stage != "event-output-split" && (*owner)[1] !=
                    attribute<std::string>(graph, writerTarget(graph, op), "grhsim.statePolicy").value_or(""))
                    return "Writer sink ownership disagrees with state policy";
            }
            const auto effectOrder = attribute<int64_t>(graph, op, "grhsim.effectOrder");
            if (operationEffect(graph, op) == OpEffect::External && (*owner)[0] == "general" && !effectOrder)
                return "General effects require explicit ordering";
            if (operationEffect(graph, op) == OpEffect::External && (*owner)[0] == "general" &&
                owner->size() > 1 && (*owner)[1] != "nonsink")
                return "General effects must remain in nonsink";
            if (effectOrder)
                if (*effectOrder < 0 || operationEffect(graph, op) != OpEffect::External ||
                    (*owner)[0] != "general" || !effectOrders.insert(*effectOrder).second)
                    return "Invalid effect ordering annotation";
            if (attribute<bool>(graph, op, "grhsim.sampleBoundary").value_or(false) &&
                (operationEffect(graph, op) != OpEffect::External || (*owner)[0] != "general"))
                return "Sample boundary requires an effectful General producer";
            for (const auto input : graph.opOperands(op)) {
                const auto producer = graph.valueDef(input);
                if (!producer) continue;
                const auto& source = pathOf(producer);
                if (source.empty()) return "Executable value depends on an unowned producer";
                if (source[0] != (*owner)[0]) {
                    const bool pulse = source[0] == "event" && isFire(graph, input);
                    const bool sample = source[0] == "general" &&
                        attribute<bool>(graph, producer, "grhsim.sampleBoundary").value_or(false) &&
                        operationEffect(graph, producer) == OpEffect::External;
                    if (!pulse && !sample) return "Ordinary computation/read crosses Event/General/Output extraction boundary";
                } else if (source[0] == "general" && source.size() > 1 && owner->size() > 1 &&
                           source[1] != "nonsink" && source != *owner)
                    return "Sink computation escapes its private partition";
            }
        }
        if (*stage != "converted" && declaration && (kind == OperationKind::kRegister || kind == OperationKind::kLatch)) {
            if (*stage != "event-output-split") {
                const auto policy = attribute<std::string>(graph, op, "grhsim.statePolicy");
                if (!policy || (*policy != "small-sink" && *policy != "large-sink")) return "State has no small/large policy";
            }
        }
        const auto* descriptor = findOpDescriptor(kind);
        if (descriptor && descriptor->dialect != "grh")
            for (const auto key : {"eventEdge", "clkPolarity", "event_edges", "TriggerSpec", "triggerMode"})
                if (has(op, key)) return "Converted op retains a raw event/trigger annotation";
    }
    if (*stage != "converted") {
        for (const auto& leaf : leaves) {
            auto parent = leaf;
            while (parent.size() > 1) {
                parent.pop_back();
                if (leaves.contains(parent)) return "Partition container also directly owns executable operations";
            }
        }
        const auto validOutput = [&](ValueId value) {
            const auto op = graph.valueDef(value);
            return !op || pathOf(op) == Owner{"output"} ||
                attribute<bool>(graph, op, "grhsim.sampleBoundary").value_or(false) ||
                (pathOf(op) == Owner{"event"} && isFire(graph, value));
        };
        for (const auto& port : graph.outputPorts()) if (!validOutput(port.value)) return "Top output was not extracted to Output";
        for (const auto& port : graph.inoutPorts())
            if (!validOutput(port.out) || !validOutput(port.oe)) return "Top inout was not extracted to Output";
        for (const auto op : graph.operations()) {
            if (attribute<std::string>(graph, op, "grhsim.history") == "output-end") {
                if (graph.opKind(op) != OperationKind::kRegister) return "Output history must be a register declaration";
                if (historyWriters[op] != 1) return "Output history must have exactly one output-end writer";
            }
        }
        if (const auto problem = partitionTopology(graph); !problem.empty()) return problem;
    }
    return {};
}

} // namespace wolvrix::lib::transform::grhsim
