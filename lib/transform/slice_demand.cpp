#include "transform/slice_demand.hpp"

#include "core/grh.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace wolvrix::lib::transform
{
    namespace
    {
        using namespace wolvrix::lib::grh;
        struct Unsupported {};
        constexpr int maxWidth = 4096;
        constexpr unsigned maxDepth = 32;
        constexpr unsigned maxExpressions = 256;

        bool pure(const Operation &op)
        {
            if (const auto attr = op.attr("hasSideEffects"))
            {
                const auto *effect = std::get_if<bool>(&*attr);
                if (!effect || *effect) return false;
            }
            return true;
        }
        int64_t integer(const Operation &op, const char *name)
        {
            const auto attr = op.attr(name);
            const auto *value = attr ? std::get_if<int64_t>(&*attr) : nullptr;
            if (!value) throw Unsupported{};
            return *value;
        }
        uint64_t mask(int width) { return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1; }

        struct Expr;
        using E = std::shared_ptr<Expr>;
        struct Expr
        {
            OperationKind kind;
            int width;
            std::vector<E> args;
            ValueId source;
            uint64_t bits = 0;
            int low = 0;
        };

        struct Planner
        {
            const Graph &graph;
            unsigned expressions = 0, shifts = 0;
            std::set<uint32_t> active;
            unsigned alignedFields = 0;

            E create(OperationKind kind, int width, std::vector<E> args = {}, uint64_t bits = 0, int low = 0)
            {
                if (++expressions > maxExpressions) throw Unsupported{};
                return std::make_shared<Expr>(Expr{kind, width, std::move(args), {}, bits, low});
            }
            E constant(int width, uint64_t bits) { return create(OperationKind::kConstant, width, {}, bits & mask(width)); }
            E ref(ValueId value)
            {
                if (!value.valid() || graph.valueType(value) != ValueType::Logic ||
                    graph.valueWidth(value) < 1 || graph.valueWidth(value) > 64) throw Unsupported{};
                auto result = create(OperationKind::kAssign, graph.valueWidth(value)); result->source = value;
                // A full-width slice changes only signedness, not bits. This
                // must precede zero extension of signed data or shift amounts.
                if (graph.getValue(value).isSigned())
                    result = create(OperationKind::kSliceStatic, result->width, {result});
                return result;
            }
            bool known(const E &e) const { return e->kind == OperationKind::kConstant && !e->source.valid(); }
            E resize(E value, int width)
            {
                if (value->width == width) return value;
                if (known(value)) return constant(width, value->bits);
                return create(width < value->width ? OperationKind::kSliceStatic : OperationKind::kAssign, width, {value});
            }
            E slice(E value, int low, int width)
            {
                if (low == 0) return resize(value, width);
                if (known(value)) return constant(width, value->bits >> low);
                return create(OperationKind::kSliceStatic, width, {value}, 0, low);
            }
            E binary(OperationKind kind, int width, E a, E b)
            {
                if (known(a) && known(b))
                {
                    uint64_t result = 0;
                    switch (kind)
                    {
                    case OperationKind::kAnd: result = a->bits & b->bits; break;
                    case OperationKind::kOr: result = a->bits | b->bits; break;
                    case OperationKind::kXor: result = a->bits ^ b->bits; break;
                    case OperationKind::kAdd: result = a->bits + b->bits; break;
                    case OperationKind::kSub: result = a->bits - b->bits; break;
                    case OperationKind::kLe: result = a->bits <= b->bits; break;
                    case OperationKind::kGe: result = a->bits >= b->bits; break;
                    case OperationKind::kLt: result = a->bits < b->bits; break;
                    case OperationKind::kEq: result = a->bits == b->bits; break;
                    case OperationKind::kShl: result = b->bits >= 64 ? 0 : a->bits << b->bits; break;
                    case OperationKind::kLShr: result = b->bits >= 64 ? 0 : a->bits >> b->bits; break;
                    default: throw Unsupported{};
                    }
                    return constant(width, result);
                }
                if ((kind == OperationKind::kShl || kind == OperationKind::kLShr) && known(b))
                {
                    if (b->bits >= 64) return constant(width, 0);
                    if (b->bits == 0) return resize(a, width);
                }
                return create(kind, width, {a, b});
            }
            E mux(E condition, E yes, E no)
            {
                if (known(condition)) return condition->bits ? yes : no;
                return create(OperationKind::kMux, yes->width, {condition, yes, no});
            }
            std::optional<slang::SVInt> literal(ValueId value)
            {
                const auto def = graph.valueDef(value);
                if (!def.valid()) return std::nullopt;
                const auto op = graph.getOperation(def);
                if (op.kind() != OperationKind::kConstant) return std::nullopt;
                if (!pure(op)) throw Unsupported{};
                const auto attr = op.attr("constValue");
                const auto *text = attr ? std::get_if<std::string>(&*attr) : nullptr;
                if (!text) throw Unsupported{};
                try
                {
                    auto parsed = slang::SVInt::fromString(*text);
                    if (parsed.hasUnknown()) throw Unsupported{};
                    parsed = parsed.resize(graph.valueWidth(value)); parsed.setSigned(false); return parsed;
                }
                catch (const std::exception &) { throw Unsupported{}; }
            }

            struct Sparse { E data; int offset; };
            Sparse sparse(ValueId value, unsigned depth)
            {
                const int width = graph.valueWidth(value);
                if (depth > maxDepth || width < 1 || width > maxWidth || graph.valueType(value) != ValueType::Logic)
                    throw Unsupported{};
                if (const auto bits = literal(value))
                {
                    const int high = static_cast<int>(bits->getActiveBits());
                    if (high == 0) return {constant(1, 0), 0};
                    int low = 0; while ((*bits)[low].value == 0) ++low;
                    if (high - low > 64) throw Unsupported{};
                    return {constant(high - low, *bits->slice(high - 1, low).as<uint64_t>()), low};
                }
                if (width <= 64) return {ref(value), 0};
                if (!active.insert(value.index).second) throw Unsupported{};
                const auto def = graph.valueDef(value); if (!def.valid()) throw Unsupported{};
                const auto op = graph.getOperation(def);
                if (!pure(op) || op.kind() != OperationKind::kConcat || op.results().size() != 1 ||
                    op.operands().empty() || op.operands().size() > 64) throw Unsupported{};
                std::vector<Sparse> pieces; int offset = 0, low = maxWidth, high = 0;
                for (auto it = op.operands().rbegin(); it != op.operands().rend(); ++it)
                {
                    auto part = sparse(*it, depth + 1); part.offset += offset;
                    if (!known(part.data) || part.data->bits != 0)
                    {
                        low = std::min(low, part.offset); high = std::max(high, part.offset + part.data->width);
                        pieces.push_back(part);
                    }
                    offset += graph.valueWidth(*it);
                    if (offset > width) throw Unsupported{};
                }
                active.erase(value.index);
                if (offset != width || high - low > 64) throw Unsupported{};
                if (pieces.empty()) return {constant(1, 0), 0};
                if (pieces.size() == 1) return pieces.front();
                auto result = constant(64, 0);
                for (const auto &part : pieces)
                    result = binary(OperationKind::kOr, 64, result,
                        binary(OperationKind::kShl, 64, resize(part.data, 64), constant(64, part.offset - low)));
                return {resize(result, high - low), low};
            }

            // Bit-pattern proof: value == +/- (zero_extend(index) << lowZeros)
            // modulo 2^width. At least one leading zero proves that the positive
            // magnitude and its negation are representable as signed values.
            struct AlignedAmount { ValueId index; int lowZeros; bool negative; int width; };
            std::optional<AlignedAmount> alignedAmount(ValueId value, unsigned depth = 0)
            {
                const int width = graph.valueWidth(value);
                if (depth > maxDepth || width < 2 || width > 64 || graph.valueType(value) != ValueType::Logic)
                    return std::nullopt;
                const auto def = graph.valueDef(value); if (!def.valid()) return std::nullopt;
                const auto op = graph.getOperation(def); const auto args = op.operands();
                if (!pure(op) || op.results().size() != 1) return std::nullopt;
                if ((op.kind() == OperationKind::kAssign || op.kind() == OperationKind::kConcat) &&
                    args.size() == 1 && graph.valueWidth(args[0]) == width)
                    return alignedAmount(args[0], depth + 1);
                if (op.kind() == OperationKind::kSub && args.size() == 2 &&
                    graph.valueWidth(args[0]) == width && graph.valueWidth(args[1]) == width)
                {
                    const auto zero = literal(args[0]);
                    if (!zero || zero->getActiveBits() != 0) return std::nullopt;
                    auto amount = alignedAmount(args[1], depth + 1);
                    if (amount) amount->negative = !amount->negative;
                    return amount;
                }
                if (op.kind() != OperationKind::kConcat || args.size() < 2 || args.size() > 64) return std::nullopt;
                ValueId index; int zeros = 0, offset = 0;
                for (auto it = args.rbegin(); it != args.rend(); ++it)
                {
                    const int w = graph.valueWidth(*it);
                    if (w < 1 || w > width - offset || graph.valueType(*it) != ValueType::Logic) return std::nullopt;
                    if (const auto bits = literal(*it))
                    {
                        if (bits->getActiveBits() != 0) return std::nullopt;
                    }
                    else
                    {
                        if (index.valid()) return std::nullopt;
                        index = *it; zeros = offset;
                    }
                    offset += w;
                }
                if (offset != width || !index.valid() || zeros == 0 || zeros >= 63 ||
                    graph.valueWidth(index) + zeros >= width) return std::nullopt;
                return AlignedAmount{index, zeros, false, width};
            }

            std::optional<E> alignedWindow(ValueId source, const AlignedAmount &amount,
                                            OperationKind direction, int low, int width, unsigned depth)
            {
                if (amount.negative) return std::nullopt;
                const auto payload = sparse(source, depth + 1);
                const uint64_t step = UINT64_C(1) << amount.lowZeros;
                if (static_cast<uint64_t>(payload.data->width) > step) return std::nullopt;
                const int delta = direction == OperationKind::kShl ? low - payload.offset : payload.offset - low;
                if (delta < 0) return std::nullopt;
                const uint64_t index = static_cast<uint64_t>(delta) / step;
                const int remainder = static_cast<int>(static_cast<uint64_t>(delta) % step);
                // The complete demanded window must fit inside one payload.
                // Then no adjacent aligned position can overlap it.
                const int bit = direction == OperationKind::kShl ? remainder : -remainder;
                if (bit < 0 || bit + width > payload.data->width) return std::nullopt;
                if (index > mask(graph.valueWidth(amount.index))) return constant(width, 0);
                ++shifts; ++alignedFields;
                return mux(binary(OperationKind::kEq, 1, ref(amount.index),
                                  constant(graph.valueWidth(amount.index), index)),
                           slice(payload.data, bit, width), constant(width, 0));
            }

            std::optional<E> alignedDirectionalMux(const Operation &op, int low, int width, unsigned depth)
            {
                const auto args = op.operands();
                if (args.size() != 3 || graph.valueWidth(args[0]) != 1) return std::nullopt;
                const auto condId = graph.valueDef(args[0]), leftId = graph.valueDef(args[1]), rightId = graph.valueDef(args[2]);
                if (!condId.valid() || !leftId.valid() || !rightId.valid()) return std::nullopt;
                const auto cond = graph.getOperation(condId), left = graph.getOperation(leftId), right = graph.getOperation(rightId);
                const auto ca = cond.operands(), la = left.operands(), ra = right.operands();
                if (!pure(cond) || !pure(left) || !pure(right) || cond.kind() != OperationKind::kLt ||
                    ca.size() != 2 || left.kind() != OperationKind::kShl || right.kind() != OperationKind::kLShr ||
                    la.size() != 2 || ra.size() != 2 || la[0] != ra[0] ||
                    graph.valueWidth(la[0]) != graph.valueWidth(args[1]) ||
                    graph.valueWidth(ra[0]) != graph.valueWidth(args[2]) ||
                    graph.valueWidth(args[1]) != graph.valueWidth(op.results()[0]) ||
                    graph.valueWidth(args[2]) != graph.valueWidth(op.results()[0]) ||
                    graph.valueWidth(ca[0]) != graph.valueWidth(ca[1]) ||
                    !graph.getValue(ca[0]).isSigned() || !graph.getValue(ca[1]).isSigned()) return std::nullopt;
                const auto zero = literal(ca[1]); if (!zero || zero->getActiveBits() != 0) return std::nullopt;
                const auto negative = alignedAmount(ca[0]), positive = alignedAmount(la[1]), rightAmount = alignedAmount(ra[1]);
                if (!negative || !positive || !rightAmount || !negative->negative || positive->negative || !rightAmount->negative ||
                    negative->index != positive->index || negative->index != rightAmount->index ||
                    negative->lowZeros != positive->lowZeros || negative->lowZeros != rightAmount->lowZeros ||
                    negative->width != positive->width || negative->width != rightAmount->width) return std::nullopt;
                // For index>0 the signed-negative test selects left shift by
                // its magnitude; for index==0 both branches shift by zero.
                return alignedWindow(la[0], *positive, OperationKind::kShl, low, width, depth);
            }

            E shifted(const Operation &op, int low, int width, unsigned depth)
            {
                const auto args = op.operands();
                if (args.size() != 2 || graph.valueWidth(args[0]) != graph.valueWidth(op.results()[0]))
                    throw Unsupported{};
                if (const auto amount = alignedAmount(args[1]))
                    if (const auto result = alignedWindow(args[0], *amount, op.kind(), low, width, depth)) return *result;
                const auto payload = sparse(args[0], depth + 1);
                const auto data = resize(payload.data, 64), amount = resize(ref(args[1]), 64);
                const int delta = low - payload.offset;
                auto displacement = constant(64, static_cast<uint64_t>(delta < 0 ? -delta : delta));
                E result;
                if (op.kind() == OperationKind::kShl && delta >= 0)
                    result = mux(binary(OperationKind::kLe, 1, amount, displacement),
                        binary(OperationKind::kLShr, 64, data, binary(OperationKind::kSub, 64, displacement, amount)),
                        binary(OperationKind::kShl, 64, data, binary(OperationKind::kSub, 64, amount, displacement)));
                else if (op.kind() == OperationKind::kLShr && delta < 0)
                    result = mux(binary(OperationKind::kGe, 1, amount, displacement),
                        binary(OperationKind::kLShr, 64, data, binary(OperationKind::kSub, 64, amount, displacement)),
                        binary(OperationKind::kShl, 64, data, binary(OperationKind::kSub, 64, displacement, amount)));
                else
                {
                    // Guard before the sum can wrap: for amount >= 64 both
                    // branches are mathematically zero in this case.
                    if ((delta < 0 ? -delta : delta) >= 64) result = constant(64, 0);
                    else result = mux(binary(OperationKind::kLt, 1, amount, constant(64, 64)),
                        binary(op.kind(), 64, data, binary(OperationKind::kAdd, 64, amount, displacement)), constant(64, 0));
                }
                ++shifts; return resize(result, width);
            }

            E window(ValueId value, int low, int width, unsigned depth = 0)
            {
                const int full = graph.valueWidth(value);
                if (depth > maxDepth || full < 1 || full > maxWidth || low < 0 || width < 1 ||
                    width > 64 || low + width > full || graph.valueType(value) != ValueType::Logic) throw Unsupported{};
                if (const auto bits = literal(value)) return constant(width, *bits->slice(low + width - 1, low).as<uint64_t>());
                if (full <= 64) return slice(ref(value), low, width);
                if (graph.getValue(value).isSigned() || !active.insert(value.index).second) throw Unsupported{};
                const auto def = graph.valueDef(value); if (!def.valid()) throw Unsupported{};
                const auto op = graph.getOperation(def); const auto args = op.operands();
                if (!pure(op) || op.results().size() != 1) throw Unsupported{};
                E result;
                switch (op.kind())
                {
                case OperationKind::kShl:
                case OperationKind::kLShr: result = shifted(op, low, width, depth); break;
                case OperationKind::kMux:
                    if (args.size() != 3 || graph.valueWidth(args[0]) != 1 ||
                        graph.valueWidth(args[1]) != full || graph.valueWidth(args[2]) != full) throw Unsupported{};
                    if (const auto aligned = alignedDirectionalMux(op, low, width, depth)) { result = *aligned; break; }
                    result = mux(ref(args[0]), window(args[1], low, width, depth + 1), window(args[2], low, width, depth + 1)); break;
                case OperationKind::kAnd:
                case OperationKind::kOr:
                case OperationKind::kXor:
                    if (args.size() != 2 || graph.valueWidth(args[0]) != full || graph.valueWidth(args[1]) != full) throw Unsupported{};
                    result = binary(op.kind(), width, window(args[0], low, width, depth + 1), window(args[1], low, width, depth + 1)); break;
                case OperationKind::kNot:
                case OperationKind::kAssign:
                    if (args.size() != 1 || graph.valueWidth(args[0]) != full) throw Unsupported{};
                    result = window(args[0], low, width, depth + 1);
                    if (op.kind() == OperationKind::kNot)
                        result = known(result) ? constant(width, ~result->bits) : create(OperationKind::kNot, width, {result});
                    break;
                case OperationKind::kConcat:
                {
                    if (args.empty() || args.size() > 64) throw Unsupported{};
                    int offset = 0; result = constant(width, 0);
                    for (auto it = args.rbegin(); it != args.rend(); ++it)
                    {
                        const int partWidth = graph.valueWidth(*it);
                        if (partWidth < 1 || partWidth > full - offset || graph.valueType(*it) != ValueType::Logic)
                            throw Unsupported{};
                        const int end = offset + partWidth;
                        const int beginBit = std::max(low, offset), endBit = std::min(low + width, end);
                        if (beginBit < endBit)
                        {
                            auto part = window(*it, beginBit - offset, endBit - beginBit, depth + 1);
                            part = binary(OperationKind::kShl, 64, resize(part, 64), constant(64, beginBit - low));
                            result = binary(OperationKind::kOr, width, result, resize(part, width));
                        }
                        offset = end;
                    }
                    if (offset != full) throw Unsupported{};
                    break;
                }
                default: throw Unsupported{};
                }
                active.erase(value.index); return result;
            }
        };

        struct Builder
        {
            Graph &graph;
            using Key = std::tuple<OperationKind, int, int, uint64_t, std::vector<uint32_t>>;
            std::map<Key, ValueId> cache;
            ValueId emit(const E &e)
            {
                if (e->source.valid()) return e->source;
                std::vector<ValueId> args; std::vector<uint32_t> ids;
                for (const auto &a : e->args) { args.push_back(emit(a)); ids.push_back(args.back().index); }
                const Key key{e->kind, e->width, e->low, e->bits, ids};
                if (const auto it = cache.find(key); it != cache.end()) return it->second;
                const auto op = graph.createOperation(e->kind); const auto value = graph.createValue(e->width, false);
                for (const auto arg : args) graph.addOperand(op, arg);
                graph.addResult(op, value);
                if (e->kind == OperationKind::kConstant)
                    graph.setAttr(op, "constValue", std::to_string(e->width) + "'d" + std::to_string(e->bits));
                if (e->kind == OperationKind::kSliceStatic)
                {
                    graph.setAttr(op, "sliceStart", int64_t(e->low)); graph.setAttr(op, "sliceEnd", int64_t(e->low + e->width - 1));
                }
                const auto loc = makeTransformSrcLoc("slice-demand", "scalar_window");
                graph.setOpSrcLoc(op, loc); graph.setValueSrcLoc(value, loc);
                cache.emplace(key, value); return value;
            }
        };
    }

    SliceDemandPass::SliceDemandPass(SliceDemandOptions options)
        : Pass("slice-demand", "slice-demand", "Propagate narrow windows through sparse wide logical shifts"), options_(options)
    {
    }
    PassResult SliceDemandPass::run()
    {
        PassResult result;
        if (!options_.twoState) return result;
        unsigned roots = 0, shifts = 0, alignedFields = 0;
        for (const auto &entry : design().graphs())
        {
            auto &graph = *entry.second; Builder builder{graph, {}};
            const std::vector<OperationId> original(graph.operations().begin(), graph.operations().end());
            for (const auto id : original)
            {
                const auto op = graph.getOperation(id);
                if (!pure(op) || op.kind() != OperationKind::kSliceStatic || op.operands().size() != 1 || op.results().size() != 1 ||
                    graph.valueWidth(op.operands()[0]) < 128 || graph.valueWidth(op.results()[0]) < 2 ||
                    graph.valueWidth(op.results()[0]) > 64 || graph.valueType(op.results()[0]) != ValueType::Logic) continue;
                try
                {
                    const auto low = integer(op, "sliceStart"), high = integer(op, "sliceEnd");
                    if (low < 0 || high < low || high >= graph.valueWidth(op.operands()[0]) ||
                        high - low + 1 != graph.valueWidth(op.results()[0])) continue;
                    Planner planner{graph, 0, 0, {}};
                    const auto plan = planner.window(op.operands()[0], static_cast<int>(low), static_cast<int>(high - low + 1));
                    if (!planner.shifts) continue;
                    const auto value = builder.emit(plan);
                    graph.replaceOperand(id, 0, value); graph.setOpKind(id, OperationKind::kAssign);
                    graph.eraseAttr(id, "sliceStart"); graph.eraseAttr(id, "sliceEnd");
                    ++roots; shifts += planner.shifts; alignedFields += planner.alignedFields; result.changed = true;
                }
                catch (const Unsupported &) { continue; }
            }
        }
        logInfo("roots=" + std::to_string(roots) + " wide_shift_windows=" + std::to_string(shifts) +
                " aligned_fields=" + std::to_string(alignedFields));
        return result;
    }
}
