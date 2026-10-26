#include "transform/grhsim/simplify.hpp"

#include "rewrite.hpp"
#include "../logic_eval.hpp"
#include "transform/verify.hpp"

#include <chrono>
#include <deque>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
using detail::Scope;

std::vector<OperationId> orderedOperations(const Graph& graph)
{
    std::vector<OperationId> result;
    const auto operations = graph.operations();
    result.reserve(operations.size());
    uint32_t last = 0;
    for (const auto op : operations) last = std::max(last, op.index);
    // This table lives only during a read-only traversal of one graph. No
    // mutation or freeze can invalidate its operation indices.
    std::vector<std::size_t> pending(static_cast<std::size_t>(last) + 1);
    for (const auto op : operations) {
        std::size_t count = 0;
        for (const auto arg : graph.opOperands(op)) count += bool(graph.valueDef(arg));
        pending[op.index] = count;
        if (!count) result.push_back(op);
    }
    for (std::size_t cursor = 0; cursor < result.size(); ++cursor) {
        const auto op = result[cursor];
        for (const auto value : graph.opResults(op)) {
            for (const auto& user : graph.valueUsers(value))
                if (--pending.at(user.operation.index) == 0) result.push_back(user.operation);
        }
    }
    if (result.size() != operations.size())
        throw std::invalid_argument("grhsim-simplify requires an acyclic expanded graph");
    return result;
}

bool commute(OperationKind kind)
{
    switch (kind) {
    case OperationKind::kAdd: case OperationKind::kMul:
    case OperationKind::kAnd: case OperationKind::kOr: case OperationKind::kXor: case OperationKind::kXnor:
    case OperationKind::kEq: case OperationKind::kNe: case OperationKind::kCaseEq: case OperationKind::kCaseNe:
    case OperationKind::kLogicAnd: case OperationKind::kLogicOr: return true;
    default: return false;
    }
}

bool makeConstant(Graph& graph, OperationId op, slang::SVInt bits)
{
    const auto result = graph.opResults(op)[0];
    bits.setSigned(graph.valueSigned(result));
    bits = bits.resize(graph.valueWidth(result));
    if (graph.valueDomain(result) == LogicDomain::TwoState) bits.flattenUnknowns();
    detail::rewrite(graph, op, OperationKind::kConstant, {});
    graph.setAttr(op, "constValue", bits.toString(slang::LiteralBase::Hex, true, bits.getBitWidth()));
    return true;
}

bool fold(Scope& scope, OperationId op)
{
    auto& graph = scope.graph;
    const auto kind = graph.opKind(op);
    if (kind == OperationKind::kConstant || operationEffect(graph, op) != OpEffect::Pure ||
        graph.opResults(op).size() != 1 || graph.valueType(graph.opResults(op)[0]) != ValueType::Logic) return false;
    // Identity aliases are handled by canonicalize. Folding a retained port or
    // declaration alias back into a constant would make CSE recreate it forever.
    if (kind == OperationKind::kAssign && graph.opOperands(op).size() == 1 &&
        graph.sameValueType(graph.opResults(op)[0], graph.opOperands(op)[0])) return false;
    std::vector<slang::SVInt> args;
    for (const auto arg : graph.opOperands(op)) {
        if (!scope.local(arg)) return false;
        const auto bits = detail::constantBits(graph, arg);
        if (!bits) return false;
        args.push_back(*bits);
    }
    if ((kind == OperationKind::kDiv || kind == OperationKind::kMod) && args.size() == 2 &&
        detail::isZero(args[1])) return false; // Keep the existing runtime division contract.
    std::optional<slang::SVInt> bits;
    if (kind == kBitSelect && args.size() == 3) bits = (args[0] & args[1]) | (~args[0] & args[2]);
    else if (kind == kPrioritySelect && args.size() % 2 == 1) {
        const auto count = (args.size() - 1) / 2;
        bits = args.back();
        for (std::size_t i = 0; i < count; ++i)
            if (!detail::isZero(args[i])) { bits = args[count + i]; break; }
    } else if (kind == kMaskedOr) {
        const auto width = graph.valueWidth(graph.opResults(op)[0]);
        bits = slang::SVInt(width, 0, graph.valueSigned(graph.opResults(op)[0]));
        if (const auto views = attribute<std::vector<int64_t>>(graph, op, "views")) {
            for (std::size_t i = 0; i < views->size(); i += 4)
                if (!detail::isZero(args[(*views)[i]].slice((*views)[i + 1], (*views)[i + 1])))
                    *bits |= args[(*views)[i + 2]].slice((*views)[i + 3] + width - 1, (*views)[i + 3]);
        } else {
            for (int32_t i = 0; i < graph.valueWidth(graph.opOperands(op)[0]); ++i)
                if (!detail::isZero(args[0].slice(i, i))) *bits |= args[1].slice((i + 1) * width - 1, i * width);
        }
    } else {
        if (kind == OperationKind::kSliceArray) {
            uint64_t linear = 0;
            const auto shape = graph.valueShape(graph.opOperands(op)[0]);
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i].hasUnknown() || args[i].isNegative() || args[i].getBitWidth() > 64) return false;
                const auto index = args[i].getRawPtr()[0];
                const auto dim = static_cast<uint64_t>(shape[i - 1]);
                if (index > INT64_MAX || linear > (INT64_MAX - index) / dim) return false;
                linear = linear * dim + index;
            }
            if (linear > INT64_MAX / graph.valueWidth(graph.opResults(op)[0])) return false;
        }
        bits = transform::detail::evaluateLogicOperation(graph, op, args,
            kind == kSystemFunction ? OperationKind::kSystemFunction : kind);
    }
    if (!bits) return false;
    const bool structural = kind == OperationKind::kAssign || kind == OperationKind::kConcat ||
        kind == OperationKind::kArrayCreate || kind == OperationKind::kSliceStatic || kind == OperationKind::kSliceArray;
    if (bits->hasUnknown() && !structural) return false;
    return makeConstant(graph, op, *bits);
}

bool canonicalize(Scope& scope, OperationId op)
{
    auto& graph = scope.graph;
    const auto kind = graph.opKind(op);
    const std::vector<ValueId> args(graph.opOperands(op).begin(), graph.opOperands(op).end());
    const auto results = graph.opResults(op);
    if (results.size() != 1 || !canCseOperation(graph, op)) return false;
    const auto result = results[0];
    const auto type = graph.valueLogicalType(result);
    const auto constant = [&](ValueId value) {
        return scope.local(value) ? detail::constantBits(graph, value) : std::nullopt;
    };
    const auto alias = [&](ValueId input) { return scope.alias(op, input); };
    if ((kind == OperationKind::kConcat || kind == OperationKind::kReplicate) &&
        args.size() == 1 && graph.valueWidth(args[0]) == type.width) return alias(args[0]);
    if (kind == OperationKind::kSliceStatic && args.size() == 1) {
        const auto lo = attribute<int64_t>(graph, op, "sliceStart").value();
        const auto hi = attribute<int64_t>(graph, op, "sliceEnd").value();
        if (lo == 0 && hi + 1 == graph.valueWidth(args[0])) return alias(args[0]);
        const auto source = graph.valueDef(args[0]);
        if (scope.contains(source) && graph.opKind(source) == OperationKind::kSliceStatic) {
            const auto start = attribute<int64_t>(graph, source, "sliceStart").value();
            detail::replaceOperands(graph, op, graph.opOperands(source));
            graph.setAttr(op, "sliceStart", start + lo); graph.setAttr(op, "sliceEnd", start + hi);
            detail::mergeOrigins(graph, op, {source}); return true;
        }
        if (scope.contains(source) && (graph.opKind(source) == OperationKind::kConcat || graph.opKind(source) == OperationKind::kArrayCreate)) {
            const auto parts = graph.opOperands(source);
            int64_t offset = 0;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                const auto part = parts[graph.opKind(source) == OperationKind::kConcat ? parts.size() - i - 1 : i];
                const auto width = graph.valueWidth(part);
                if (lo >= offset && hi < offset + width) {
                    if (lo == offset && hi + 1 == offset + width) return alias(part);
                    detail::replaceOperands(graph, op, std::array{part});
                    graph.setAttr(op, "sliceStart", lo - offset); graph.setAttr(op, "sliceEnd", hi - offset);
                    detail::mergeOrigins(graph, op, {source}); return true;
                }
                offset += width;
            }
        }
    }
    if (kind == OperationKind::kSliceArray && args.size() >= 2) {
        const auto source = graph.valueDef(args[0]);
        const auto index = constant(args[1]);
        if (scope.contains(source) && graph.opKind(source) == OperationKind::kArrayCreate && index &&
            !index->hasUnknown() && !index->isNegative()) {
            const auto row = index->as<uint64_t>();
            const auto elements = graph.opOperands(source);
            if (row && *row < elements.size()) {
                if (args.size() == 2) return alias(elements[*row]);
                // sliceArray linearizes indices before checking the flattened range.
                // An inner out-of-range index can therefore select a later row.
                const auto shape = graph.valueShape(args[0]);
                bool innerInBounds = true;
                for (std::size_t i = 2; i < args.size(); ++i) {
                    const auto inner = constant(args[i]);
                    const auto offset = inner && !inner->hasUnknown() && !inner->isNegative()
                        ? inner->as<uint64_t>() : std::nullopt;
                    if (!offset || *offset >= static_cast<uint64_t>(shape[i - 1])) {
                        innerInBounds = false;
                        break;
                    }
                }
                if (innerInBounds) {
                    std::vector<ValueId> inputs{elements[*row]}; inputs.insert(inputs.end(), args.begin() + 2, args.end());
                    detail::replaceOperands(graph, op, inputs); detail::mergeOrigins(graph, op, {source}); return true;
                }
            }
        }
    }
    if (kind == OperationKind::kAssign && args.size() == 1 && !type.shape.empty()) {
        const auto source = graph.valueDef(args[0]);
        if (scope.contains(source) && graph.opKind(source) == OperationKind::kConcat) {
            const auto parts = graph.opOperands(source);
            if (!parts.empty()) {
                auto elementType = graph.valueLogicalType(parts[0]);
                auto arrayType = elementType;
                arrayType.width = graph.valueWidth(args[0]);
                arrayType.shape.insert(arrayType.shape.begin(), static_cast<int64_t>(parts.size()));
                if (arrayType == type && std::all_of(parts.begin(), parts.end(), [&](auto value) {
                    return graph.valueLogicalType(value) == elementType;
                })) {
                    const std::vector<ValueId> inputs(parts.rbegin(), parts.rend());
                    detail::rewrite(graph, op, OperationKind::kArrayCreate, inputs); detail::mergeOrigins(graph, op, {source}); return true;
                }
            }
        }
    }
    if (kind == OperationKind::kAssign && args.size() == 1) return alias(args[0]);
    if (!detail::twoState(type)) return false;
    if ((kind == OperationKind::kMux || kind == kBitSelect) && args.size() == 3) {
        if (args[1] == args[2]) return alias(args[1]);
        if (const auto cond = constant(args[0])) {
            if (detail::isZero(*cond)) return alias(args[2]);
            if (kind == OperationKind::kMux || detail::isOnes(*cond)) return alias(args[1]);
        }
        const auto bit = [&](ValueId value) { const auto t = graph.valueLogicalType(value);
            return t == LogicalType{ValueType::Logic, 1, false, LogicDomain::TwoState, {}}; };
        if (kind == OperationKind::kMux && bit(result) && std::all_of(args.begin(), args.end(), bit)) {
            detail::rewrite(graph, op, kBitSelect, args); return true;
        }
    }
    if ((kind == OperationKind::kNot || kind == OperationKind::kLogicNot) && args.size() == 1) {
        const auto source = graph.valueDef(args[0]);
        if (scope.contains(source) && graph.opKind(source) == kind &&
            (kind == OperationKind::kNot || (type.width == 1 && graph.valueWidth(graph.opOperands(source)[0]) == 1)))
            return alias(graph.opOperands(source)[0]);
    }
    if (args.size() == 2 && (kind == OperationKind::kLogicAnd || kind == OperationKind::kLogicOr) &&
        type.width == 1 && !type.isSigned && type.shape.empty() &&
        graph.sameValueType(result, args[0]) && graph.sameValueType(result, args[1])) {
        detail::rewrite(graph, op, kind == OperationKind::kLogicAnd ? OperationKind::kAnd : OperationKind::kOr, args); return true;
    }
    if (args.size() != 2) return false;
    if ((kind == OperationKind::kAnd || kind == OperationKind::kOr) && args[0] == args[1]) return alias(args[0]);
    if (commute(kind) && graph.sameValueType(args[0], args[1]) &&
        std::tie(args[0].index, args[0].generation) > std::tie(args[1].index, args[1].generation)) {
        detail::replaceOperands(graph, op, std::array{args[1], args[0]}); return true;
    }
    for (std::size_t i = 0; i < 2; ++i) {
        const auto bits = constant(args[i]);
        if (!bits) continue;
        const auto other = args[1 - i];
        if (detail::isZero(*bits)) {
            if (kind == OperationKind::kOr || kind == OperationKind::kXor || kind == OperationKind::kAdd ||
                (kind == OperationKind::kSub && i == 1) ||
                ((kind == OperationKind::kShl || kind == OperationKind::kLShr || kind == OperationKind::kAShr) && i == 1))
                return alias(other);
            if (kind == OperationKind::kAnd || kind == OperationKind::kMul) return makeConstant(graph, op, slang::SVInt(type.width, 0, type.isSigned));
        }
        if (detail::isOnes(*bits) && kind == OperationKind::kAnd && bits->getBitWidth() == type.width) return alias(other);
    }
    return false;
}

bool muxChains(Scope& scope)
{
    auto& graph = scope.graph;
    bool changed = false;
    const auto eligible = [&](OperationId op) {
        if (!scope.contains(op) || graph.opKind(op) != OperationKind::kMux ||
            graph.opOperands(op).size() != 3 || graph.opResults(op).size() != 1) return false;
        const auto type = graph.valueLogicalType(graph.opResults(op)[0]);
        const auto cond = graph.valueLogicalType(graph.opOperands(op)[0]);
        return detail::twoState(type) && type.shape.empty() && type.width <= 64 &&
            cond == LogicalType{ValueType::Logic, 1, false, LogicDomain::TwoState, {}} &&
            graph.sameValueType(graph.opResults(op)[0], graph.opOperands(op)[1]) &&
            graph.sameValueType(graph.opResults(op)[0], graph.opOperands(op)[2]);
    };
    std::vector<OperationId> buffer;
    const auto operations = scope.operationRange(buffer);
    for (const auto op : operations) {
        if (!eligible(op)) continue;
        std::vector<OperationId> chain{op};
        while (chain.size() < 64) {
            const auto tail = graph.opOperands(chain.back())[2];
            const auto next = graph.valueDef(tail);
            if (!eligible(next) || scope.exported(tail) || scope.anchored(next) ||
                graph.valueUsers(tail).size() != 1) break;
            chain.push_back(next);
        }
        if (chain.size() < 3) continue;
        std::vector<ValueId> args;
        for (const auto link : chain) args.push_back(graph.opOperands(link)[0]);
        for (const auto link : chain) args.push_back(graph.opOperands(link)[1]);
        args.push_back(graph.opOperands(chain.back())[2]);
        for (const auto link : chain) detail::mergeOrigins(graph, op, {link});
        detail::rewrite(graph, op, kPrioritySelect, args);
        changed = true;
    }
    return changed;
}

// Transient CSE keys need equality, not printable serialization. Keep the
// persistent clock signature's textual format in expressionKey unchanged.
class CseKey {
public:
    template <typename T> void scalar(T value) {
        static_assert(std::is_arithmetic_v<T>);
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
    void text(std::string_view value) {
        scalar(static_cast<uint64_t>(value.size())); bytes.append(value);
    }
    void type(const Graph& graph, ValueId value) {
        scalar(static_cast<uint8_t>(graph.valueType(value)));
        scalar(graph.valueWidth(value)); scalar(graph.valueSigned(value));
        scalar(static_cast<uint8_t>(graph.valueDomain(value)));
        const auto shape = graph.valueShape(value);
        scalar(static_cast<uint64_t>(shape.size()));
        for (const auto dimension : shape) scalar(dimension);
    }
    void attribute(const AttributeValue& attribute) {
        scalar(static_cast<uint8_t>(attribute.index()));
        const auto element = [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>) text(value);
            else scalar(value);
        };
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string> || std::is_arithmetic_v<T>) element(value);
            else {
                scalar(static_cast<uint64_t>(value.size()));
                for (const auto& entry : value) element(entry);
            }
        }, attribute);
    }
    std::string bytes;
};

std::string cseKey(const Graph& graph, OperationId op) {
    CseKey key;
    key.bytes.reserve(96);
    const bool history = graph.opKind(op) == kEdgeDetect;
    key.scalar(static_cast<uint32_t>(graph.opKind(op)));
    const auto operands = graph.opOperands(op), results = graph.opResults(op);
    key.scalar(static_cast<uint64_t>(operands.size()));
    for (const auto value : operands) {
        key.scalar(value.index); key.scalar(value.generation); key.type(graph, value);
    }
    key.scalar(static_cast<uint64_t>(results.size()));
    for (const auto value : results) key.type(graph, value);
    std::vector<const AttrKV*> attributes;
    for (const auto& entry : graph.opAttrs(op))
        if (entry.key != kPartition && entry.key != kOriginModulesKey &&
            !(history && entry.key == "prevInitFallback")) attributes.push_back(&entry);
    std::sort(attributes.begin(), attributes.end(), [](const auto* left, const auto* right) { return left->key < right->key; });
    for (const auto* entry : attributes) {
        key.text(entry->key);
        if (history && entry->key == "prevInit")
            key.attribute(slang::SVInt::fromString(std::get<std::string>(entry->value)).toString());
        else key.attribute(entry->value);
    }
    return std::move(key.bytes);
}

bool foldAndShare(Scope& scope, std::span<const OperationId> ordered)
{
    auto& graph = scope.graph;
    bool changed = false;
    std::unordered_map<std::string, ValueId> expressions;
    for (const auto op : ordered) {
        if (!scope.contains(op)) continue;
        // Keep named declaration/port views, but do not make their consumers
        // wait another whole-graph round to see the shared source. In a leaf
        // scope both the alias and its source must remain inside that owner.
        const auto operandCount = graph.opOperands(op).size();
        for (std::size_t i = 0; i < operandCount; ++i) {
            const auto original = graph.opOperands(op)[i];
            auto input = original;
            while (const auto source = graph.valueDef(input)) {
                if (!scope.contains(source) || graph.opKind(source) != OperationKind::kAssign) break;
                const auto args = graph.opOperands(source);
                if (args.size() != 1 || !graph.sameValueType(input, args[0]) ||
                    (!scope.partition.empty() && !scope.local(args[0]))) break;
                input = args[0];
            }
            if (input != original) {
                graph.replaceOperand(op, i, input);
                changed = true;
            }
        }
        if (graph.opResults(op).size() != 1) continue;
        if (fold(scope, op)) changed = true;
        const auto symbol = graph.operationSymbol(op);
        if (canonicalize(scope, op)) {
            changed = true;
            if (graph.findOperation(symbol) != op) continue;
        }
        const auto result = graph.opResults(op)[0];
        // A retained identity assignment is a named view, not a new
        // expression. CSE of these views grows a dependency chain by one
        // alias per round even though their shared source is already unique.
        if (graph.opKind(op) == OperationKind::kAssign && graph.opOperands(op).size() == 1 &&
            graph.sameValueType(result, graph.opOperands(op)[0])) continue;
        if (!canCseOperation(graph, op) && graph.opKind(op) != kEdgeDetect) continue;
        if (ordered.size() == 1) continue; // No second expression in this leaf can share it.
        const auto key = cseKey(graph, op);
        const auto [it, inserted] = expressions.emplace(key, result);
        if (!inserted && it->second != result) {
            detail::mergeOrigins(graph, graph.valueDef(it->second), {op});
            changed = scope.alias(op, it->second) || changed;
        }
    }
    return changed;
}
} // namespace

SimplifyPass::SimplifyPass(SimplifyOptions options)
    : Pass("grhsim-simplify", "grhsim-simplify", "Simplify within complete virtual partition owners"),
      options_(std::move(options)) {}

bool SimplifyPass::supportsDialect(std::string_view dialect) const noexcept { return detail::supports(dialect); }

PassResult SimplifyPass::run()
{
    PassResult result;
    try {
        if (options_.maxRounds == 0) throw std::invalid_argument("max-rounds must be positive");
        auto& graph = detail::selectGraph(design(), options_.top);
        PassManager verifier; verifier.addPass(std::make_unique<GrhVerifyPass>());
        if (!verifier.run(design(), diags()).success) return {false, true, {}};
        auto ordered = orderedOperations(graph); // Check cycles before the first mutation.
        std::unordered_set<uint32_t> anchors;
        for (const auto symbol : graph.declaredSymbols()) anchors.insert(symbol.value);
        for (const auto& group : graph.generateGroups())
            for (const auto symbol : group.symbols) anchors.insert(symbol.value);
        uint32_t round = 0;
        std::size_t ownerCount = 0;
        bool changed = false;
        do {
            using Clock = std::chrono::steady_clock;
            const auto started = Clock::now();
            std::array<Clock::duration, 6> elapsed{};
            std::array<bool, 6> updates{};
            const auto timed = [&](std::size_t index, auto&& action) {
                const auto begin = Clock::now();
                const bool updated = action();
                elapsed[index] += Clock::now() - begin;
                updates[index] = updated || updates[index];
                changed = updated || changed;
            };
            changed = false;
            struct Member { const detail::Owner* owner; OperationId operation; std::size_t order; };
            std::vector<Member> members;
            members.reserve(ordered.size());
            for (std::size_t i = 0; i < ordered.size(); ++i) {
                const auto op = ordered[i];
                if (operationEffect(graph, op) != OpEffect::Declaration)
                    members.push_back({&detail::ownerView(graph, op), op, i});
            }
            const auto less = [](const auto& left, const auto& right) {
                if (*left.owner != *right.owner) return *left.owner < *right.owner;
                return left.order < right.order;
            };
            if (!std::is_sorted(members.begin(), members.end(), less))
                std::sort(members.begin(), members.end(), less);
            // Borrow owners only during the immutable grouping traversal.
            // Other leaves cannot erase this group's representative. Resolve
            // and copy its key just before rewriting that leaf, after any
            // mutations that might have relocated attribute storage.
            struct Group { OperationId representative; std::size_t begin, end; };
            std::vector<Group> groups;
            std::vector<OperationId> grouped;
            grouped.reserve(members.size());
            const detail::Owner* previous = nullptr;
            for (const auto& member : members) {
                if (!previous || *previous != *member.owner)
                    groups.push_back({member.operation, grouped.size(), grouped.size()});
                grouped.push_back(member.operation);
                groups.back().end = grouped.size();
                previous = member.owner;
            }
            ownerCount = groups.size();
            members.clear();
            for (const auto& group : groups) {
                const auto owner = detail::owner(graph, group.representative);
                const auto operations = std::span<const OperationId>(grouped).subspan(group.begin, group.end - group.begin);
                // Partitioned scopes visit their own members. New operations
                // are collected with their owner at the next rewrite round.
                Scope scope(graph, owner, owner.empty() ? std::nullopt : std::optional(operations), &anchors, &owner);
                timed(0, [&] { return foldAndShare(scope, operations); });
                timed(1, [&] { return muxChains(scope); });
                if (owner.empty()) timed(2, [&] { return detail::mergeEquivalentStates(scope); });
                timed(3, [&] { return scope.dce(); });
                timed(4, [&] { return detail::simplifyUsedBits(scope); });
            }
            result.changed = result.changed || changed;
            ++round;
            if (changed) timed(5, [&] { ordered = orderedOperations(graph); return false; });
            const auto milliseconds = [](auto duration) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
            };
            std::ostringstream timing;
            timing << "round=" << round << " changed=" << changed
                   << " elapsed-ms=" << milliseconds(Clock::now() - started)
                   << " fold-share-ms=" << milliseconds(elapsed[0])
                   << " mux-ms=" << milliseconds(elapsed[1])
                   << " state-merge-ms=" << milliseconds(elapsed[2])
                   << " dce-ms=" << milliseconds(elapsed[3])
                   << " used-bits-ms=" << milliseconds(elapsed[4])
                   << " topo-ms=" << milliseconds(elapsed[5])
                   << " updates=";
            for (std::size_t i = 0; i < 5; ++i) timing << (updates[i] ? '1' : '0');
            logInfo(timing.str());
        } while (changed && round < options_.maxRounds);
        if (changed) warning("Reached max-rounds before convergence");
        logInfo("rounds=" + std::to_string(round) + " owners=" + std::to_string(ownerCount));
        if (!verifier.run(design(), diags()).success) result.failed = true;
    } catch (const std::exception& error) {
        this->error(error.what()); result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform::grhsim
