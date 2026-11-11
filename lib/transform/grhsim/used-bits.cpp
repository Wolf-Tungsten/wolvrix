#include "rewrite.hpp"
#include "transform/grhsim/used-bits.hpp"

#include "transform/verify.hpp"

#include <deque>
#include <limits>

namespace wolvrix::lib::transform::grhsim::detail {
namespace {
struct StateUses {
    OperationId declaration;
    LogicalType type;
    InitSpec init;
    std::vector<OperationId> reads, writes;
    bool eligible = true;
    uint32_t used = 0;
};

bool stateReadKind(OperationKind kind)
{
    return kind == OperationKind::kRegisterReadPort || kind == OperationKind::kRegisterReadArrayPort ||
        kind == OperationKind::kLatchReadPort;
}

bool stateWriteKind(OperationKind kind)
{
    return kind == kWrite || kind == kWriteMask || kind == kWriteArray || kind == kWriteArrayMask ||
        kind == kFill || kind == kLatchWrite ||
        kind == OperationKind::kRegisterWritePort || kind == OperationKind::kRegisterWriteMaskPort ||
        kind == OperationKind::kRegisterWriteArrayPort || kind == OperationKind::kRegisterWriteArrayMaskPort ||
        kind == OperationKind::kRegisterFillPort;
}

// Return data/mask operands for both ordinary GRH writes and lowered GrhSIM
// writes.  The F22 passes run before conversion, so the ordinary port forms
// are the common case; retaining the lowered forms keeps the helper useful
// for standalone compatibility tests.
std::vector<std::size_t> stateWriteDataOperands(const Graph& graph, OperationId op)
{
    const auto kind = graph.opKind(op);
    if (kind == kWrite || kind == kWriteMask || kind == kWriteArray || kind == kWriteArrayMask ||
        kind == kFill || kind == kLatchWrite) {
        // Converted GrhSIM writers have [fire, updateCond, indices..., data,
        // mask?, events...].  The state data and mask are the only operands
        // whose width follows the used state prefix.
        const auto args = graph.opOperands(op);
        std::size_t eventCount = attribute<std::vector<std::string>>(graph, op, "eventEdge").value_or(std::vector<std::string>{}).size();
        const std::size_t eventStart = args.size() >= eventCount ? args.size() - eventCount : args.size();
        if (kind == kLatchWrite) return eventStart >= 3 ? std::vector<std::size_t>{eventStart - 2, eventStart - 1} : std::vector<std::size_t>{};
        if (kind == kWriteMask || kind == kWriteArrayMask)
            return eventStart >= 2 ? std::vector<std::size_t>{eventStart - 2, eventStart - 1} : std::vector<std::size_t>{};
        return eventStart >= 1 ? std::vector<std::size_t>{eventStart - 1} : std::vector<std::size_t>{};
    }
    const auto args = graph.opOperands(op);
    if (kind == OperationKind::kRegisterFillPort) return args.size() > 1 ? std::vector<std::size_t>{1} : std::vector<std::size_t>{};
    const auto eventStart = firstEventOperandIndex(graph, op);
    if (!eventStart) return {};
    if (kind == OperationKind::kRegisterWriteMaskPort || kind == OperationKind::kRegisterWriteArrayMaskPort ||
        kind == OperationKind::kLatchWritePort)
        return *eventStart >= 2 ? std::vector<std::size_t>{*eventStart - 2, *eventStart - 1} : std::vector<std::size_t>{};
    return *eventStart >= 1 ? std::vector<std::size_t>{*eventStart - 1} : std::vector<std::size_t>{};
}

struct StateTable {
    static constexpr uint32_t absent = std::numeric_limits<uint32_t>::max();
    std::vector<std::pair<OperationId, StateUses>> entries;
    // Invocation-local indices for the initial declarations and their ports.
    // New operations are never queried here; nothing survives a graph rewrite.
    std::vector<uint32_t> byOperation;

    auto begin() { return entries.begin(); }
    auto end() { return entries.end(); }
    StateUses& at(OperationId target) {
        const auto index = target.index < byOperation.size() ? byOperation[target.index] : absent;
        if (index == absent || entries[index].first != target)
            throw std::invalid_argument("State port target does not resolve");
        return entries[index].second;
    }
    StateUses& forPort(OperationId port) {
        const auto index = port.index < byOperation.size() ? byOperation[port.index] : absent;
        if (index == absent) throw std::invalid_argument("State port target does not resolve");
        return entries[index].second;
    }
};

StateTable stateUses(Scope& scope)
{
    auto& graph = scope.graph;
    StateTable states;
    if (!scope.partition.empty()) return states;
    uint32_t lastOperation = 0;
    for (const auto op : graph.operations()) {
        lastOperation = std::max(lastOperation, op.index);
        if (graph.opKind(op) != OperationKind::kRegister && graph.opKind(op) != OperationKind::kLatch) continue;
        StateUses state{op, stateLogicalType(graph, op), stateInitSpec(graph, op)};
        // F22 executes on the ordinary GRH before grh-to-grhsim projects the
        // logic domain to two-state.  Prefix demand is bit-local for scalar
        // logic in either domain, so retain the same narrowing opportunity
        // for four-state FIRRTL input; unknown bits are preserved by the
        // literal resize below.
        state.eligible = scope.partition.empty() && state.type.kind == ValueType::Logic &&
            state.type.shape.empty();
        // GSIM can narrow scalar state with either a literal or a random
        // initializer.  Random initialization is width-parametric at the
        // emitter boundary, so it does not need a literal resize below.
        for (const auto& step : state.init)
            state.eligible &= step.kind == InitKind::Constant || step.kind == InitKind::Random;
        states.entries.emplace_back(op, std::move(state));
    }
    std::sort(states.entries.begin(), states.entries.end(), [](const auto& left, const auto& right) {
        return left.first.index < right.first.index;
    });
    states.byOperation.resize(static_cast<std::size_t>(lastOperation) + 1, StateTable::absent);
    for (uint32_t i = 0; i < states.entries.size(); ++i) states.byOperation[states.entries[i].first.index] = i;
    for (const auto op : graph.operations()) {
        auto symbol = attribute<std::string>(graph, op, "regSymbol");
        if (!symbol) symbol = attribute<std::string>(graph, op, "latchSymbol");
        if (!symbol) continue;
        const auto target = graph.findOperation(*symbol);
        auto& state = states.at(target);
        states.byOperation[op.index] = states.byOperation[target.index];
        const auto kind = graph.opKind(op);
        state.eligible &= scope.contains(op);
        if (stateReadKind(kind)) state.reads.push_back(op);
        else if (stateWriteKind(kind)) state.writes.push_back(op);
        else state.eligible = false;
    }
    return states;
}

bool prefixValue(const Graph& graph, ValueId value) {
    return graph.valueType(value) == ValueType::Logic &&
        graph.valueShape(value).empty();
}

bool rebuildable(const Graph& graph, OperationId op)
{
    const auto kind = graph.opKind(op);
    const auto args = graph.opOperands(op);
    switch (kind) {
    case OperationKind::kConstant: case OperationKind::kSliceStatic: return true;
    case OperationKind::kAssign:
        return args.size() == 1 && graph.valueShape(args[0]).empty();
    case OperationKind::kConcat:
        return std::all_of(args.begin(), args.end(), [&](auto value) { return graph.valueShape(value).empty(); });
    case OperationKind::kAdd: case OperationKind::kSub: case OperationKind::kMul: {
        // FourState arithmetic propagates any operand X/Z into the retained
        // low bits too, so a low result prefix still depends on whole inputs.
        const auto results = graph.opResults(op);
        return results.size() == 1 && graph.valueDomain(results[0]) == LogicDomain::TwoState &&
            std::all_of(args.begin(), args.end(), [&](auto value) {
                return prefixValue(graph, value) && graph.valueDomain(value) == LogicDomain::TwoState;
            });
    }
    case OperationKind::kAnd: case OperationKind::kOr: case OperationKind::kXor: case OperationKind::kXnor:
    case OperationKind::kNot:
    case OperationKind::kShl: case OperationKind::kMux: case OperationKind::kReplicate:
        return std::all_of(args.begin(), args.end(), [&](auto value) { return prefixValue(graph, value); });
    default: return kind == kBitSelect || kind == kPrioritySelect;
    }
}
}

bool mergeEquivalentStates(Scope& scope)
{
    if (!scope.partition.empty()) return false;
    auto& graph = scope.graph;
    auto states = stateUses(scope);
    std::unordered_map<std::string, OperationId> representatives;
    bool changed = false;
    for (auto& [target, state] : states) {
        if (!state.eligible || state.writes.size() != 1 || scope.anchored(state.writes[0])) continue;
        const auto writer = state.writes[0];
        std::ostringstream key;
        encode(key, state.type);
        for (const auto& step : state.init) encode(key, AttributeValue(step.value));
        key << static_cast<unsigned>(graph.opKind(writer)) << ';';
        for (const auto input : graph.opOperands(writer)) key << input.index << ',' << input.generation << ';';
        for (const auto entry : graph.opAttrRange(writer))
            if (entry.key() != "regSymbol" && entry.key() != "latchSymbol" && entry.key() != "writerOrder" &&
                entry.key() != kOriginModulesKey) { key << entry.key() << ':'; encode(key, entry.value()); }
        const auto [it, inserted] = representatives.emplace(key.str(), target);
        if (inserted) continue;
        const auto canonical = it->second;
        const auto name = std::string(graph.getOperation(canonical).symbolText());
        const auto reference = graph.opKind(target) == OperationKind::kLatch ? "latchSymbol" : "regSymbol";
        // Include the declaration kind in the equivalence check (register vs latch).
        if (graph.opKind(canonical) != graph.opKind(target)) continue;
        for (const auto read : state.reads) graph.setAttr(read, reference, name);
        mergeOrigins(graph, canonical, {target});
        const auto canonicalWriter = states.at(canonical).writes[0];
        mergeOrigins(graph, canonicalWriter, {writer});
        graph.eraseOp(writer);
        if (scope.anchored(target)) {
            const auto read = graph.createOperation(graph.opKind(target) == OperationKind::kLatch
                ? OperationKind::kLatchReadPort : OperationKind::kRegisterReadPort);
            const auto value = graph.createValue(state.type);
            graph.addResult(read, value); graph.setAttr(read, reference, name);
            inheritOrigin(graph, read, {target, canonical});
            replaceStateWithView(graph, target, value, 0);
        } else graph.eraseOp(target);
        changed = true;
    }
    return changed;
}

bool simplifyUsedBits(Scope& scope)
{
    auto& graph = scope.graph;
    std::vector<OperationId> buffer;
    const auto operations = scope.operationRange(buffer);
    if (!scope.partition.empty() && std::all_of(operations.begin(), operations.end(), [&](auto op) {
        return std::all_of(graph.opResults(op).begin(), graph.opResults(op).end(), [&](auto value) {
            return scope.exported(value) || scope.anchored(op);
        });
    })) return false; // All result bits are observable outside this leaf.
    struct ValueDemand {
        int32_t width = 0;
        uint32_t bits = 0;
        bool prefix = false;
        bool remembered = false;
    };
    // A whole-graph round touches virtually every value repeatedly. Its
    // indices remain local to this invocation; leaf scopes use a sparse table
    // so a one-op partition never allocates a whole-graph workspace.
    const bool dense = scope.partition.empty();
    uint32_t lastValue = 0, lastOperation = 0;
    if (dense) for (const auto op : operations) {
        lastOperation = std::max(lastOperation, op.index);
        for (const auto value : graph.opOperands(op)) lastValue = std::max(lastValue, value.index);
        for (const auto value : graph.opResults(op)) lastValue = std::max(lastValue, value.index);
    }
    std::vector<ValueDemand> denseOriginal(dense ? static_cast<std::size_t>(lastValue) + 1 : 0);
    std::unordered_map<ValueId, ValueDemand, ValueIdHash> sparseOriginal;
    if (!dense) sparseOriginal.reserve(operations.size());
    const auto demand = [&](ValueId value) -> ValueDemand& {
        return dense ? denseOriginal.at(value.index) : sparseOriginal.at(value);
    };
    const auto remember = [&](ValueId value) {
        auto& entry = dense ? denseOriginal.at(value.index) : sparseOriginal[value];
        if (!entry.remembered) {
            entry.width = graph.valueWidth(value);
            entry.prefix = prefixValue(graph, value);
            entry.remembered = true;
        }
    };
    for (const auto op : operations) {
        for (const auto value : graph.opOperands(op)) remember(value);
        for (const auto value : graph.opResults(op)) remember(value);
    }
    auto states = stateUses(scope);
    for (auto& [target, state] : states) if (scope.anchored(target)) state.eligible = false;
    std::deque<OperationId> queue;
    std::unordered_set<OperationId, OperationIdHash> queued;
    if (!dense) queued.reserve(operations.size());
    std::vector<uint8_t> denseQueued(dense ? static_cast<std::size_t>(lastOperation) + 1 : 0);
    const auto enqueue = [&](OperationId op) {
        if (!op) return;
        if (dense) {
            if (!denseQueued.at(op.index)) { denseQueued[op.index] = 1; queue.push_back(op); }
        } else if (queued.insert(op).second) queue.push_back(op);
    };
    const auto raise = [&](ValueId value, uint32_t bits) {
        auto& valueDemand = demand(value);
        bits = std::min<uint32_t>(bits, valueDemand.width);
        if (!bits || valueDemand.bits >= bits) return;
        if (!valueDemand.prefix) bits = valueDemand.width;
        valueDemand.bits = bits;
        if (const auto producer = graph.valueDef(value); scope.contains(producer)) enqueue(producer);
    };
    const auto full = [&](ValueId value) { raise(value, demand(value).width); };
    for (const auto op : operations) {
        if (!scope.contains(op)) continue;
        const auto effect = operationEffect(graph, op);
        if (effect != OpEffect::Pure && effect != OpEffect::ReadState && effect != OpEffect::Declaration) enqueue(op);
        for (const auto value : graph.opResults(op))
            if (scope.exported(value) || scope.anchored(op)) full(value);
    }
    while (!queue.empty()) {
        const auto op = queue.front(); queue.pop_front();
        if (dense) denseQueued[op.index] = 0; else queued.erase(op);
        const auto args = graph.opOperands(op), results = graph.opResults(op);
        const auto kind = graph.opKind(op);
        uint32_t used = 0;
        for (const auto result : results) used = std::max(used, demand(result).bits);
        if (kind == OperationKind::kConstant) continue;
        if (stateReadKind(kind)) {
            // Array-read indices select storage rows; their high bits remain
            // observable even when the read result only needs a low prefix.
            for (const auto arg : args) full(arg);
            if (!scope.partition.empty()) continue;
            auto& state = states.forPort(op);
            if (used > state.used) {
                state.used = used;
                for (const auto writer : state.writes) if (scope.contains(writer)) enqueue(writer);
            }
            continue;
        }
        if (stateWriteKind(kind)) {
            if (!scope.partition.empty()) { for (const auto arg : args) full(arg); continue; }
            auto& state = states.forPort(op);
            const auto bits = state.eligible ? state.used : state.type.width;
            const auto dataOperands = stateWriteDataOperands(graph, op);
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (std::find(dataOperands.begin(), dataOperands.end(), i) != dataOperands.end())
                    raise(args[i], bits);
                else full(args[i]);
            }
            continue;
        }
        if (used == 0 && operationEffect(graph, op) == OpEffect::Pure) continue;
        if (!rebuildable(graph, op) || (results.size() && !demand(results[0]).prefix)) {
            for (const auto arg : args) full(arg);
            continue;
        }
        if (kind == OperationKind::kSliceStatic) {
            raise(args[0], attribute<int64_t>(graph, op, "sliceStart").value() + used);
        } else if (kind == OperationKind::kConcat) {
            uint32_t offset = 0;
            for (std::size_t i = args.size(); i-- > 0;) {
                if (used > offset) raise(args[i], used - offset);
                offset += demand(args[i]).width;
            }
        } else if (kind == OperationKind::kMux || kind == kPrioritySelect) {
            const auto count = kind == OperationKind::kMux ? 1 : (args.size() - 1) / 2;
            for (std::size_t i = 0; i < args.size(); ++i) i < count ? full(args[i]) : raise(args[i], used);
        } else if (kind == OperationKind::kShl) { raise(args[0], used); full(args[1]); }
        else for (const auto arg : args) raise(arg, used);
    }

    bool changed = false;
    std::unordered_map<OperationId, uint32_t, OperationIdHash> stateWidth;
    for (auto& [target, state] : states) {
        if (!state.eligible || state.used >= static_cast<uint32_t>(state.type.width)) continue;
        if (state.used == 0) {
            // UsedBits only records demand. The caller's explicit DCE owns
            // removal of an entirely unused state/write cone.
            continue;
        } else {
            auto type = state.type; type.width = state.used;
            for (auto& step : state.init) if (step.kind == InitKind::Constant) {
                auto bits = slang::SVInt::fromString(step.value);
                bits.setSigned(type.isSigned); bits = bits.resize(type.width);
                step.value = bits.toString(slang::LiteralBase::Hex, true, bits.getBitWidth());
            }
            setStateLogicalType(graph, target, type); setStateInitSpec(graph, target, state.init);
            stateWidth.emplace(target, state.used); changed = true;
        }
    }
    std::unordered_map<OperationId, uint32_t, OperationIdHash> narrow;
    for (const auto op : operations) {
        if (!scope.contains(op) || graph.opResults(op).size() != 1) continue;
        const auto result = graph.opResults(op)[0];
        const auto& original = demand(result);
        uint32_t width = demand(result).bits;
        if (stateReadKind(graph.opKind(op))) {
            if (!dense) continue;
            const auto target = states.forPort(op).declaration;
            if (!stateWidth.contains(target)) continue;
            width = stateWidth.at(target);
        } else if (!rebuildable(graph, op) || operationEffect(graph, op) != OpEffect::Pure) continue;
        if (!width || width >= static_cast<uint32_t>(original.width) || !original.prefix || scope.exported(result) || scope.anchored(op)) continue;
        auto type = graph.valueLogicalType(result);
        type.width = width; graph.setValueLogicalType(result, type); narrow.emplace(op, width); changed = true;
    }
    // Without a narrowed value or state, every operand already has its
    // original width. DCE is intentionally a separate pass boundary.
    if (!changed) return false;
    for (const auto op : operations) {
        if (!scope.contains(op)) continue;
        const auto kind = graph.opKind(op);
        const std::vector<ValueId> args(graph.opOperands(op).begin(), graph.opOperands(op).end());
        if (stateWriteKind(kind)) {
            if (!dense) continue;
            const auto found = stateWidth.find(states.forPort(op).declaration);
            if (found != stateWidth.end()) {
                for (const auto index : stateWriteDataOperands(graph, op))
                    if (index < args.size()) graph.replaceOperand(op, index,
                        lowBits(graph, op, args[index], found->second, "used-bits"));
            }
            continue;
        }
        if (graph.opResults(op).size() != 1) continue;
        const auto width = graph.valueWidth(graph.opResults(op)[0]);
        if (!rebuildable(graph, op)) continue;
        if (kind == OperationKind::kConstant) {
            if (narrow.contains(op)) {
                auto bits = slang::SVInt::fromString(attribute<std::string>(graph, op, "constValue").value());
                bits.setSigned(graph.valueSigned(graph.opResults(op)[0])); bits = bits.resize(width);
                graph.setAttr(op, "constValue", bits.toString(slang::LiteralBase::Hex, true, bits.getBitWidth()));
            }
        } else if (kind == OperationKind::kSliceStatic) {
            if (narrow.contains(op)) graph.setAttr(op, "sliceEnd", attribute<int64_t>(graph, op, "sliceStart").value() + width - 1);
        } else if (kind == OperationKind::kConcat) {
            int32_t remaining = width;
            std::vector<ValueId> parts;
            for (std::size_t i = args.size(); i-- > 0 && remaining > 0;) {
                const auto keep = std::min(remaining, demand(args[i]).width);
                parts.push_back(lowBits(graph, op, args[i], keep, "used-bits")); remaining -= keep;
            }
            std::reverse(parts.begin(), parts.end());
            if (parts != args) { replaceOperands(graph, op, parts); changed = true; }
        } else if (kind == OperationKind::kReplicate && narrow.contains(op)) {
            const auto stride = demand(args[0]).width;
            if (width <= stride) rewrite(graph, op, OperationKind::kAssign,
                        std::array{lowBits(graph, op, args[0], width, "used-bits")});
            else if (width % stride == 0) graph.setAttr(op, "rep", static_cast<int64_t>(width / stride));
            else {
                auto type = graph.valueLogicalType(graph.opResults(op)[0]); type.width = width - width % stride;
                const auto low = makeOperation(graph, op, OperationKind::kReplicate, type,
                                               std::array{args[0]}, "used-bits");
                graph.setAttr(graph.valueDef(low), "rep", static_cast<int64_t>(width / stride));
                const auto high = lowBits(graph, op, args[0], width % stride, "used-bits");
                rewrite(graph, op, OperationKind::kConcat, std::array{high, low});
            }
        } else if (narrow.contains(op)) {
            const auto count = kind == OperationKind::kMux ? 1 : kind == kPrioritySelect ? (args.size() - 1) / 2 : 0;
            for (std::size_t i = 0; i < args.size(); ++i)
                if (i >= count && !(kind == OperationKind::kShl && i == 1))
                    graph.replaceOperand(op, i, lowBits(graph, op, args[i], width, "used-bits"));
        }
    }
    return changed;
}

} // namespace wolvrix::lib::transform::grhsim::detail

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;

void requireGrhWhole(Graph& graph)
{
    if (graph.metadata(kStage))
        throw std::invalid_argument("used-bits requires an unpartitioned GRH graph before grh-to-grhsim");
    for (const auto op : graph.operations())
        if (attribute<std::vector<std::string>>(graph, op, kPartition))
            throw std::invalid_argument("used-bits requires an unpartitioned GRH graph");
}

Graph& selectGrhGraph(Design& design, std::string_view top)
{
    if (top.empty()) {
        if (design.topGraphs().size() != 1) throw std::invalid_argument("Select exactly one top graph with -top");
        top = design.topGraphs()[0];
    }
    auto* graph = design.findGraph(top);
    if (!graph) throw std::invalid_argument("Top graph does not exist: " + std::string(top));
    return *graph;
}

void verifyWhole(Graph& graph)
{
    if (const auto problem = verifyGraph(graph); !problem.empty()) throw std::invalid_argument(problem);
    for (const auto op : graph.operations())
        if (const auto problem = verifyOpDescriptor(graph, op); !problem.empty())
            throw std::invalid_argument(problem);
}

} // namespace

UsedBitsPass::UsedBitsPass(UsedBitsOptions options)
    : Pass("used-bits", "used-bits", "GSIM-compatible backward used-bit analysis"),
      options_(std::move(options))
{}

bool UsedBitsPass::supportsDialect(std::string_view dialect) const noexcept
{
    return detail::supports(dialect);
}

PassResult UsedBitsPass::run()
{
    PassResult result;
    try {
        auto& graph = selectGrhGraph(design(), options_.top);
        requireGrhWhole(graph);
        verifyWhole(graph);
        const auto beforeOperations = graph.operations().size();
        const auto beforeValues = graph.values().size();
        std::size_t beforeWidth = 0;
        for (const auto value : graph.values()) beforeWidth += static_cast<std::size_t>(std::max(0, graph.valueWidth(value)));

        // See ExprOptPass: an empty anchor set is the F22 default.  The
        // operation/value roots used by GSIM are represented by ports,
        // state/effect operations and exported values in Scope itself.
        const std::unordered_set<uint32_t> noAnchors;
        const auto* anchors = keepDeclaredSymbols() ? nullptr : &noAnchors;
        detail::Scope scope(graph, {}, std::nullopt, anchors, nullptr, true);
        const bool narrowed = detail::simplifyUsedBits(scope);
        std::size_t afterWidth = 0;
        for (const auto value : graph.values()) afterWidth += static_cast<std::size_t>(std::max(0, graph.valueWidth(value)));
        result.changed = narrowed || beforeOperations != graph.operations().size() ||
            beforeValues != graph.values().size() || beforeWidth != afterWidth;
        verifyWhole(graph);
        const auto summary = "narrowed=" + std::to_string(narrowed ? 1 : 0) +
            " operations=" + std::to_string(beforeOperations) + "->" + std::to_string(graph.operations().size()) +
            " values=" + std::to_string(beforeValues) + "->" + std::to_string(graph.values().size()) +
            " width-sum=" + std::to_string(beforeWidth) + "->" + std::to_string(afterWidth) +
            " semantic-pruned=0";
        info(summary);
        logInfo(summary);
    } catch (const std::exception& error) {
        this->error(error.what());
        result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform::grhsim
