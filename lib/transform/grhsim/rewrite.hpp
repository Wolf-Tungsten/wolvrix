#ifndef WOLVRIX_TRANSFORM_GRHSIM_REWRITE_HPP
#define WOLVRIX_TRANSFORM_GRHSIM_REWRITE_HPP

#include "transform/grhsim/ops.hpp"
#include "core/transform.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <set>
#include <sstream>

namespace wolvrix::lib::transform::grhsim::detail {
using namespace grh;
using Owner = std::vector<std::string>;

inline bool supports(std::string_view dialect) {
    return dialect == "grh" || dialect == "grhsim" || dialect == "grhsim.cpu";
}

inline Graph& selectGraph(Design& design, std::string_view top) {
    if (top.empty()) {
        if (design.topGraphs().size() != 1) throw std::invalid_argument("Select exactly one top graph with -top");
        top = design.topGraphs()[0];
    }
    auto* graph = design.findGraph(top);
    if (!graph) throw std::invalid_argument("Top graph does not exist: " + std::string(top));
    if (!graph->metadata(kStage)) throw std::invalid_argument("Run grh-to-grhsim before simulation optimizations");
    // All callers immediately run GrhVerifyPass, which includes verifyGraph
    // and every descriptor. Do not repeat those full-graph traversals here.
    return *graph;
}

inline Owner owner(const Graph& graph, OperationId op) {
    return attribute<Owner>(graph, op, kPartition).value_or(Owner{});
}

// Valid only until graph mutation. Never persist this view in a session slot.
inline const Owner& ownerView(const Graph& graph, OperationId op) {
    static const Owner empty;
    for (const auto& entry : graph.opAttrs(op)) if (entry.key == kPartition) {
        if (const auto* path = std::get_if<Owner>(&entry.value)) return *path;
        throw std::invalid_argument("Wrong attribute type: " + std::string(kPartition));
    }
    return empty;
}

inline bool twoState(const LogicalType& type) {
    return type.kind == ValueType::Logic && type.domain == LogicDomain::TwoState;
}

inline std::optional<slang::SVInt> constantBits(const Graph& graph, ValueId value) {
    const auto op = graph.valueDef(value);
    if (!op || graph.opKind(op) != OperationKind::kConstant || graph.valueType(value) != ValueType::Logic) return {};
    const auto literal = attribute<std::string>(graph, op, "constValue");
    if (!literal) return {};
    try {
        auto bits = slang::SVInt::fromString(*literal);
        bits.setSigned(graph.valueSigned(value));
        bits = bits.resize(graph.valueWidth(value));
        if (graph.valueDomain(value) == LogicDomain::TwoState) bits.flattenUnknowns();
        return bits;
    } catch (const std::exception&) { return {}; }
}

inline bool isZero(const slang::SVInt& bits) { return !bits.hasUnknown() && bits == 0; }
inline bool isOnes(const slang::SVInt& bits) { return !bits.hasUnknown() && (~bits) == 0; }

inline void replaceOperands(Graph& graph, OperationId op, std::span<const ValueId> operands) {
    // The input may alias this operation's own operand storage. Snapshot it
    // before editing, and leave unchanged uses in place (notably clock/enable
    // values whose fanout can span the complete expanded graph).
    const std::vector<ValueId> replacement(operands.begin(), operands.end());
    while (graph.opOperands(op).size() > replacement.size())
        graph.eraseOperand(op, graph.opOperands(op).size() - 1);
    const auto common = graph.opOperands(op).size();
    for (std::size_t i = 0; i < common; ++i)
        graph.replaceOperand(op, i, replacement[i]);
    for (std::size_t i = common; i < replacement.size(); ++i)
        graph.addOperand(op, replacement[i]);
}

inline void rewrite(Graph& graph, OperationId op, OperationKind kind, std::span<const ValueId> operands) {
    std::vector<std::string> erase;
    for (const auto& attr : graph.opAttrs(op))
        if (attr.key != kPartition && attr.key != kOriginModulesKey) erase.emplace_back(attr.key);
    for (const auto& key : erase) graph.eraseAttr(op, key);
    replaceOperands(graph, op, operands);
    graph.setOpKind(op, kind);
}

inline void mergeOrigins(Graph& graph, OperationId target, std::initializer_list<OperationId> sources) {
    for (const auto source : sources)
        if (source)
            if (const auto origins = getOriginModules(graph, source)) mergeOriginModules(graph, target, *origins);
}

inline ValueId makeOperation(Graph& graph, OperationId source, OperationKind kind,
                             const LogicalType& type, std::span<const ValueId> operands) {
    const auto op = graph.createOperation(kind);
    const auto result = graph.createValue(type);
    for (const auto input : operands) graph.addOperand(op, input);
    graph.addResult(op, result);
    if (const auto partition = attribute<Owner>(graph, source, kPartition)) graph.setAttr(op, kPartition, *partition);
    inheritOrigin(graph, op, {source});
    graph.setOpSrcLoc(op, makeTransformSrcLoc("grhsim-simplify"));
    return result;
}

inline ValueId lowBits(Graph& graph, OperationId source, ValueId input, int32_t width) {
    if (graph.valueWidth(input) <= width) return input;
    auto type = graph.valueLogicalType(input);
    if (type.kind != ValueType::Logic || !type.shape.empty()) throw std::invalid_argument("Cannot prefix-narrow an aggregate");
    type.width = width;
    const auto value = makeOperation(graph, source, OperationKind::kSliceStatic, type, std::array{input});
    graph.setAttr(graph.valueDef(value), "sliceStart", int64_t{0});
    graph.setAttr(graph.valueDef(value), "sliceEnd", int64_t{width - 1});
    return value;
}

inline void encode(std::ostream& out, const AttributeValue& attribute) {
    out << attribute.index() << ':';
    const auto element = [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) out << value.size() << ':' << value;
        else out << std::setprecision(17) << value << ';';
    };
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string> || std::is_arithmetic_v<T>) element(value);
        else { out << value.size() << '['; for (const auto& entry : value) element(entry); out << ']'; }
    }, attribute);
}

inline void encode(std::ostream& out, const LogicalType& type) {
    out << static_cast<unsigned>(type.kind) << ',' << type.width << ',' << type.isSigned << ','
        << static_cast<unsigned>(type.domain) << ',';
    for (const auto dim : type.shape) out << dim << ',';
    out << ';';
}

inline std::string expressionKey(const Graph& graph, OperationId op, bool history = false) {
    std::ostringstream out;
    out << static_cast<unsigned>(graph.opKind(op)) << ';';
    for (const auto value : graph.opOperands(op)) {
        out << value.index << ',' << value.generation << ';';
        encode(out, graph.valueLogicalType(value));
    }
    out << '/';
    for (const auto value : graph.opResults(op)) encode(out, graph.valueLogicalType(value));
    std::vector<std::pair<std::string, AttributeValue>> attrs;
    for (const auto& entry : graph.opAttrs(op)) {
        if (entry.key == kPartition || entry.key == kOriginModulesKey || (history && entry.key == "prevInitFallback")) continue;
        auto value = entry.value;
        if (history && entry.key == "prevInit") value = slang::SVInt::fromString(std::get<std::string>(value)).toString();
        attrs.emplace_back(entry.key, std::move(value));
    }
    std::sort(attrs.begin(), attrs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [key, value] : attrs) { out << key.size() << ':' << key; encode(out, value); }
    return out.str();
}

struct Scope {
    Graph& graph;
    Owner ownedPartition;
    const Owner& partition;
    std::unordered_set<uint32_t> anchors;
    const std::unordered_set<uint32_t>* sharedAnchors = nullptr;
    std::optional<std::span<const OperationId>> members;
    std::unordered_set<OperationId, OperationIdHash> erased;

    Scope(Graph& graph, const Owner& partition, std::optional<std::span<const OperationId>> members = std::nullopt,
          const std::unordered_set<uint32_t>* sharedAnchors = nullptr, const Owner* sharedPartition = nullptr)
        : graph(graph), ownedPartition(sharedPartition ? Owner{} : partition),
          partition(sharedPartition ? *sharedPartition : ownedPartition),
          sharedAnchors(sharedAnchors), members(members) {
        if (!sharedAnchors) {
            for (const auto symbol : graph.declaredSymbols()) anchors.insert(symbol.value);
            for (const auto& group : graph.generateGroups())
                for (const auto symbol : group.symbols) anchors.insert(symbol.value);
        }
    }
    std::vector<OperationId> operations() const {
        if (!members) return {graph.operations().begin(), graph.operations().end()};
        std::vector<OperationId> result;
        for (const auto op : *members) if (!erased.contains(op)) result.push_back(op);
        return result;
    }
    std::span<const OperationId> operationRange(std::vector<OperationId>& buffer) const {
        if (members && erased.empty()) return *members;
        buffer = operations();
        return buffer;
    }
    bool contains(OperationId op) const {
        if (!op || erased.contains(op)) return false;
        for (const auto& entry : graph.opAttrs(op)) if (entry.key == kPartition) {
            if (const auto* path = std::get_if<Owner>(&entry.value)) return *path == partition;
            throw std::invalid_argument("Wrong attribute type: " + std::string(kPartition));
        }
        return partition.empty();
    }
    bool local(ValueId value) const { return contains(graph.valueDef(value)); }
    bool anchored(ValueId value) const { return (sharedAnchors ? *sharedAnchors : anchors).contains(graph.valueSymbol(value).value); }
    bool anchored(OperationId op) const { return (sharedAnchors ? *sharedAnchors : anchors).contains(graph.operationSymbol(op).value); }
    bool exported(ValueId value) const {
        if (graph.valueIsInput(value) || graph.valueIsOutput(value) || graph.valueIsInout(value) || anchored(value)) return true;
        // The converted graph is one complete scope: no use can escape it.
        if (partition.empty()) return false;
        for (const auto& use : graph.valueUsers(value)) if (!contains(use.operation)) return true;
        return false;
    }
    bool alias(OperationId op, ValueId input) {
        const auto result = graph.opResults(op)[0];
        if (result == input) return false;
        if (!graph.sameValueType(result, input)) {
            const auto type = graph.valueLogicalType(result), from = graph.valueLogicalType(input);
            if (type.width != from.width || type.kind != from.kind || type.domain != from.domain) return false;
        }
        const auto args = graph.opOperands(op);
        if (graph.opKind(op) == OperationKind::kAssign && args.size() == 1 && args[0] == input &&
            (exported(result) || anchored(op) || !local(input) || !graph.sameValueType(result, input))) return false;
        if (exported(result) || anchored(op) || !local(input) || !graph.sameValueType(result, input)) {
            rewrite(graph, op, OperationKind::kAssign, std::array{input});
        } else {
            mergeOrigins(graph, graph.valueDef(input), {op});
            graph.replaceAllUses(result, input);
            graph.eraseOp(op);
            erased.insert(op);
            graph.eraseValue(result);
        }
        return true;
    }
    bool dce() {
        bool changed = false;
        auto queue = operations();
        while (!queue.empty()) {
            const auto op = queue.back(); queue.pop_back();
            if (!contains(op) || anchored(op)) continue;
            const auto effect = operationEffect(graph, op);
            if (effect != OpEffect::Pure && effect != OpEffect::ReadState) continue;
            const auto outputs = graph.opResults(op);
            if (std::any_of(outputs.begin(), outputs.end(), [&](auto value) {
                return !graph.valueUsers(value).empty() || exported(value);
            })) continue;
            const std::vector<ValueId> results(outputs.begin(), outputs.end());
            const std::vector<ValueId> args(graph.opOperands(op).begin(), graph.opOperands(op).end());
            if (!graph.eraseOp(op)) continue;
            erased.insert(op);
            for (const auto value : results) graph.eraseValue(value);
            for (const auto arg : args) if (const auto producer = graph.valueDef(arg)) queue.push_back(producer);
            changed = true;
        }
        return changed;
    }
};

bool simplifyUsedBits(Scope& scope);
bool mergeEquivalentStates(Scope& scope);

// Transfer the original declaration symbol to a live value view. All state
// ports must already reference the replacement declaration or that view.
inline ValueId replaceStateWithView(Graph& graph, OperationId declaration, ValueId source, int32_t offset) {
    const auto type = stateLogicalType(graph, declaration);
    const auto symbol = graph.operationSymbol(declaration);
    const auto location = graph.opSrcLoc(declaration);
    graph.setOpSymbol(declaration, graph.makeInternalOpSym());
    const auto value = graph.createValue(symbol, type);
    const auto op = graph.createOperation(type.width == graph.valueWidth(source) && offset == 0
        ? OperationKind::kAssign : OperationKind::kSliceStatic);
    graph.addOperand(op, source); graph.addResult(op, value);
    if (graph.opKind(op) == OperationKind::kSliceStatic) {
        graph.setAttr(op, "sliceStart", static_cast<int64_t>(offset));
        graph.setAttr(op, "sliceEnd", static_cast<int64_t>(offset + type.width - 1));
    }
    if (location) { graph.setOpSrcLoc(op, *location); graph.setValueSrcLoc(value, *location); }
    inheritOrigin(graph, op, {declaration});
    graph.eraseOp(declaration);
    return value;
}

} // namespace wolvrix::lib::transform::grhsim::detail
#endif
