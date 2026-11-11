#include "semantic.hpp"
#include "core/state.hpp"
#include "slang/util/SmallVector.h"

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <set>
#include <utility>

namespace wolvrix::lib::ingest::firrtl::detail {
namespace {
using grh::LogicalType;
using grh::OperationKind;
using grh::ValueId;
using LeafKey = std::pair<std::string, std::vector<std::string>>;

struct AccessPath {
    std::string_view name;
    template <typename T, std::size_t InlineCapacity>
    class InlinePath {
    public:
        void push_back(const T& value) {
            if (heap_.empty() && size_ < InlineCapacity) {
                inline_[size_++] = value;
                return;
            }
            if (heap_.empty()) {
                heap_.reserve(InlineCapacity * 2);
                heap_.insert(heap_.end(), inline_.begin(), inline_.end());
            }
            heap_.push_back(value);
            ++size_;
        }
        template <typename Range>
        void append(const Range& values) {
            for (const auto& value : values) push_back(value);
        }
        bool empty() const noexcept { return size_ == 0; }
        std::size_t size() const noexcept { return size_; }
        const T& operator[](std::size_t index) const noexcept {
            return heap_.empty() ? inline_[index] : heap_[index];
        }
        std::span<const T> span() const noexcept {
            return heap_.empty() ? std::span<const T>(inline_.data(), size_)
                                 : std::span<const T>(heap_.data(), heap_.size());
        }
    private:
        std::array<T, InlineCapacity> inline_{};
        std::vector<T> heap_;
        std::size_t size_ = 0;
    };
    InlinePath<std::string_view, 4> fields;
    InlinePath<ExprId, 2> indices;
};
struct DriverPiece {
    int64_t end = 0;
    ValueId value;
};
using DriverPieces = std::map<int64_t, DriverPiece>;
struct Driver {
    Leaf leaf;
    ValueId value;
    ValueId current;
    Range range;
    // Disjoint half-open bit intervals, with a sentinel at total width.
    std::map<int64_t, int> coverage;
    // Current packed provenance.  Static indexed writes replace pieces in
    // this map, so a later field overwrite does not retain the old aggregate
    // value merely as an operand of a historical kConcat.
    std::unique_ptr<DriverPieces> pieces;
    std::string regSymbol;
    ValueId memoryMask;
    grh::SemanticId semantic = 0;
};

struct LeafKeyView {
    std::string_view name;
    std::span<const std::string> fields;
};

struct LeafKeyBorrowedView {
    std::string_view name;
    std::span<const std::string_view> fields;
};

struct LeafKeyLess {
    using is_transparent = void;

    static std::string_view name(const LeafKey& key) { return key.first; }
    static std::string_view name(const LeafKeyView& key) { return key.name; }
    static std::string_view name(const LeafKeyBorrowedView& key) { return key.name; }
    static const std::vector<std::string>& fields(const LeafKey& key) { return key.second; }
    static std::span<const std::string> fields(const LeafKeyView& key) { return key.fields; }
    static std::span<const std::string_view> fields(const LeafKeyBorrowedView& key) { return key.fields; }

    template <typename LeftFields, typename RightFields>
    static bool less(std::string_view leftName, const LeftFields& leftFields,
                     std::string_view rightName, const RightFields& rightFields) {
        if (leftName != rightName) return leftName < rightName;
        const auto count = std::min(leftFields.size(), rightFields.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto left = std::string_view(leftFields[i]);
            const auto right = std::string_view(rightFields[i]);
            if (left != right) return left < right;
        }
        return leftFields.size() < rightFields.size();
    }

    template <typename Left, typename Right>
    bool operator()(const Left& left, const Right& right) const {
        return less(name(left), fields(left), name(right), fields(right));
    }
};

class Lowerer {
public:
    Lowerer(Semantics& semantics, grh::Graph& graph, const FirrtlOptions& options)
        : semantics_(semantics), graph_(graph), options_(options), sourceFile_(semantics.source.path().generic_string()) {}
    ModuleSignature run();
private:
    Semantics& semantics_;
    grh::Graph& graph_;
    const FirrtlOptions& options_;
    // Source locations are attached to every generated op/value, but the
    // source file and line-level FIRRTL info are shared by a large number of
    // generated operations. Keep owned strings here so location()
    // avoids rescanning the source for each operation on the same line.
    std::string sourceFile_;
    mutable uint64_t locationLine_ = 0;
    mutable std::string locationSourceInfo_;
    std::map<LeafKey, Driver, LeafKeyLess> drivers_;
    // Empty field paths (including arrays) use one hash lookup. Both the key
    // storage and Driver addresses belong to stable, never-erased map nodes.
    std::unordered_map<std::string_view, Driver*> rootDrivers_;
    std::unordered_map<std::string_view, grh::SemanticId> semanticDeclarations_;
    std::map<int, ValueId> guards_;
    grh::SemanticId currentSemantic_ = 0;
    class SemanticScope {
    public:
        SemanticScope(grh::SemanticId& current, grh::SemanticId semantic)
            : current_(current), previous_(std::exchange(current, semantic)) {}
        ~SemanticScope() { current_ = previous_; }
    private:
        grh::SemanticId& current_;
        grh::SemanticId previous_;
    };
    void setOpSemantic(grh::OperationId operation);
    void setOpSemantic(grh::OperationId operation, grh::SemanticId semantic);
    grh::SemanticId generatedSemantic(std::string name);
    Driver* rootDriver(std::string_view name) const;
    Driver& findDriver(std::string_view name, std::span<const std::string> fields);
    const Driver& findDriver(std::string_view name, std::span<const std::string> fields) const;
    Driver& findDriver(std::string_view name, std::span<const std::string_view> fields);
    const Driver& findDriver(std::string_view name, std::span<const std::string_view> fields) const;
    grh::SrcLocView location(Range range) const;
    LogicalType logical(const Leaf& leaf) const;
    const Leaf& projected(ExprId expression, const std::vector<std::string>& fields) const;
    ValueId op(OperationKind kind, const LogicalType& type, std::span<const ValueId> args, Range range);
    ValueId op(OperationKind kind, const LogicalType& type, std::initializer_list<ValueId> args, Range range) {
        return op(kind, type, std::span(args.begin(), args.size()), range);
    }
    ValueId constant(const LogicalType& type, std::string bits, Range range);
    ValueId number(int64_t value, int32_t width, Range range);
    ValueId invalid(const LogicalType& type, Range range);
    ValueId view(ValueId value, const LogicalType& type, Range range);
    ValueId adapt(ValueId value, const LogicalType& type, Range range);
    ValueId slice(ValueId value, int64_t start, int64_t width, bool sign, Range range);
    bool access(ExprId expression, AccessPath& path) const;
    ValueId index(ExprId expression);
    ValueId inBounds(ValueId index, int64_t extent, Range range);
    ValueId read(ValueId base, std::span<const ExprId> indices, Range range);
    ValueId stateRead(const Driver& driver, std::span<const ExprId> indices, Range range);
    void stateWrite(const Driver& driver, const Declaration::Register& state, ValueId enable,
                    const std::vector<ValueId>& indices, ValueId data, Range range);
    ValueId expression(ExprId id, const std::vector<std::string>& fields = {});
    ValueId primitive(ExprId id, const LogicalType& type);
    ValueId guard(int id, Range range);
    void connect(const Connection& connection);
    void cover(Driver& driver, int64_t begin, int64_t end, int guard);
    void drive(ValueId target, ValueId source, Range range, bool root = false);
    void checkCycles() const;
    void memories();
    bool memorySource(const LeafKey& key) const;
    ValueId pipeline(ValueId data, ValueId clock, int64_t stages, Range range);
    void effects();
    void instances();
};

grh::SrcLocView Lowerer::location(Range range) const {
    grh::SrcLocView result;
    result.file = sourceFile_;
    result.line = static_cast<uint32_t>(range.line);
    result.column = static_cast<uint32_t>(range.column);
    std::string_view sourceInfo = range.info;
    if (sourceInfo.empty()) {
        if (locationLine_ == range.line) {
            sourceInfo = locationSourceInfo_;
        }
        else {
            locationLine_ = range.line;
            locationSourceInfo_ = sourceInfoAt(semantics_.source, range);
            sourceInfo = locationSourceInfo_;
        }
    }
    result.pass = "ingest-firrtl"; result.note = sourceInfo;
    return result;
}
Driver* Lowerer::rootDriver(std::string_view name) const {
    const auto found = rootDrivers_.find(name);
    if (found == rootDrivers_.end()) throw std::out_of_range("map::at");
    return found->second;
}
Driver& Lowerer::findDriver(std::string_view name, std::span<const std::string> fields) {
    if (fields.empty()) return *rootDriver(name);
    const auto found = drivers_.find(LeafKeyView{name, fields});
    if (found == drivers_.end()) throw std::out_of_range("map::at");
    return found->second;
}
const Driver& Lowerer::findDriver(std::string_view name, std::span<const std::string> fields) const {
    if (fields.empty()) return *rootDriver(name);
    const auto found = drivers_.find(LeafKeyView{name, fields});
    if (found == drivers_.end()) throw std::out_of_range("map::at");
    return found->second;
}
Driver& Lowerer::findDriver(std::string_view name, std::span<const std::string_view> fields) {
    if (fields.empty()) return *rootDriver(name);
    const auto found = drivers_.find(LeafKeyBorrowedView{name, fields});
    if (found == drivers_.end()) throw std::out_of_range("map::at");
    return found->second;
}
const Driver& Lowerer::findDriver(std::string_view name, std::span<const std::string_view> fields) const {
    if (fields.empty()) return *rootDriver(name);
    const auto found = drivers_.find(LeafKeyBorrowedView{name, fields});
    if (found == drivers_.end()) throw std::out_of_range("map::at");
    return found->second;
}
LogicalType Lowerer::logical(const Leaf& leaf) const {
    return {grh::ValueType::Logic, static_cast<int32_t>(semantics_.total(leaf)),
            semantics_.resolvedKind(leaf.type) == TypeKind::SInt, grh::LogicDomain::FourState, leaf.shape};
}
const Leaf& Lowerer::projected(ExprId id, const std::vector<std::string>& fields) const {
    const auto& leaves = semantics_.leavesCached(semantics_.expressions[id]);
    if (fields.empty() && leaves.size() == 1 && leaves.front().fields.empty()) return leaves.front();
    const auto found = std::find_if(leaves.begin(), leaves.end(), [&](const auto& leaf) { return leaf.fields == fields; });
    if (found == leaves.end()) throw std::logic_error("invalid FIRRTL expression leaf");
    return *found;
}
void Lowerer::setOpSemantic(grh::OperationId operation) {
    setOpSemantic(operation, currentSemantic_);
}
void Lowerer::setOpSemantic(grh::OperationId operation, grh::SemanticId semantic) {
    if (semantic) graph_.setOpSemanticIds(operation, std::span<const grh::SemanticId>(&semantic, 1));
}
grh::SemanticId Lowerer::generatedSemantic(std::string name) {
    auto* tree = graph_.semanticTree();
    if (!tree) return 0;
    return tree->addNode(tree->root(), grh::SemanticNodeKind::Expression, name);
}
ValueId Lowerer::op(OperationKind kind, const LogicalType& type, std::span<const ValueId> args, Range range) {
    if (!type.width) return {};

    const auto operation = graph_.createTemporaryOperation(kind);

    const auto result = graph_.createTemporaryValue(type);

    if (args.size() > 1) graph_.reserveOpOperandCapacity(operation, args.size());
    for (const auto value : args) {
        if (!value) throw std::logic_error("zero-width FIRRTL operand was not eliminated");
        graph_.addOperand(operation, value);
    }

    graph_.addResult(operation, result);

    // Both entities borrow the same source text and share one pooled record.
    graph_.setOpAndValueSrcLocView(operation, result, location(range));

    setOpSemantic(operation);
    return result;
}
ValueId Lowerer::constant(const LogicalType& type, std::string bits, Range range) {
    const auto value = op(OperationKind::kConstant, type, {}, range);
    if (value) graph_.setAttr(graph_.valueDef(value), "constValue", std::to_string(type.width) + (type.isSigned ? "'sb" : "'b") + bits);
    return value;
}
ValueId Lowerer::number(int64_t value, int32_t width, Range range) {
    auto unsignedValue = static_cast<uint64_t>(value);
    std::string bits;
    do { bits += char('0' + (unsignedValue & 1)); unsignedValue >>= 1; } while (unsignedValue);
    std::reverse(bits.begin(), bits.end());
    if (bits.size() > static_cast<std::size_t>(width)) bits.erase(0, bits.size() - width);
    return constant({grh::ValueType::Logic, width, false}, std::move(bits), range);
}
ValueId Lowerer::invalid(const LogicalType& type, Range range) {
    return constant(type, options_.invalidPolicy == FirrtlInvalidPolicy::Unknown ? "x" : "0", range);
}
ValueId Lowerer::view(ValueId value, const LogicalType& type, Range range) {
    if (graph_.valueLogicalType(value) == type) return value;
    return op(OperationKind::kAssign, type, {value}, range);
}
ValueId Lowerer::slice(ValueId value, int64_t start, int64_t width, bool sign, Range range) {
    if (!width) return {};
    const LogicalType type{grh::ValueType::Logic, static_cast<int32_t>(width), sign};
    if (start == 0 && graph_.valueWidth(value) == width) return view(value, type, range);
    const auto result = op(OperationKind::kSliceStatic, type, {value}, range);
    graph_.setAttr(graph_.valueDef(result), "sliceStart", start);
    graph_.setAttr(graph_.valueDef(result), "sliceEnd", start + width - 1);
    return result;
}
ValueId Lowerer::adapt(ValueId value, const LogicalType& type, Range range) {
    if (!type.width) return {};
    if (!value) return constant(type, "0", range);
    const auto original = graph_.valueLogicalType(value);
    if (original.width == type.width && original.shape == type.shape) return view(value, type, range);
    if (original.shape != type.shape) throw std::logic_error("FIRRTL width adaptation changed array geometry");
    if (!type.shape.empty()) {
        auto childType = type; childType.width /= type.shape[0]; childType.shape.erase(childType.shape.begin());
        std::vector<ValueId> children;
        children.reserve(type.shape[0]);
        for (int64_t i = 0; i < type.shape[0]; ++i) {
            auto sourceType = original; sourceType.width /= original.shape[0]; sourceType.shape.erase(sourceType.shape.begin());
            const auto child = op(OperationKind::kSliceArray, sourceType, {value, number(i, 32, range)}, range);
            children.push_back(adapt(child, childType, range));
        }
        return op(OperationKind::kArrayCreate, type, children, range);
    }
    if (original.width > type.width) return slice(value, 0, type.width, type.isSigned, range);
    const auto extra = type.width - original.width;
    ValueId high;
    if (original.isSigned) {
        high = slice(value, original.width - 1, 1, false, range);
        if (extra > 1) {
            const auto repeated = op(OperationKind::kReplicate, {grh::ValueType::Logic, extra, false}, {high}, range);
            graph_.setAttr(graph_.valueDef(repeated), "rep", int64_t(extra)); high = repeated;
        }
    } else high = number(0, extra, range);
    return op(OperationKind::kConcat, type, {high, value}, range);
}

bool Lowerer::access(ExprId id, AccessPath& path) const {
    const auto& expr = semantics_.body.arena.expressions[id];

    if (expr.kind == ExprKind::Reference) {
        path.name = expr.name; return true;
    }
    if (expr.kind != ExprKind::Field && expr.kind != ExprKind::Index) return false;
    if (!access(expr.operands[0], path)) return false;
    if (expr.kind == ExprKind::Field) {
        path.fields.push_back(expr.name);
    } else {
        path.indices.push_back(expr.operands[1]);
    }
    return true;
}
ValueId Lowerer::index(ExprId id) {
    const auto& expr = semantics_.body.arena.expressions[id];
    if (expr.kind == ExprKind::Integer) return number(*semantics_.staticIndex(id), 32, expr.range);
    const auto result = expression(id);
    return result ? result : number(0, 1, expr.range);
}
ValueId Lowerer::inBounds(ValueId value, int64_t extent, Range range) {
    const auto width = std::max(graph_.valueWidth(value), static_cast<int32_t>(std::bit_width(static_cast<uint64_t>(extent))));
    const LogicalType type{grh::ValueType::Logic, width, false};
    return op(OperationKind::kLt, {grh::ValueType::Logic, 1, false}, {adapt(value, type, range), number(extent, width, range)}, range);
}
ValueId Lowerer::read(ValueId base, std::span<const ExprId> indices, Range range) {
    if (indices.empty() || !base) return base;
    const auto original = graph_.valueLogicalType(base);
    auto resultType = original;
    for (std::size_t i = 0; i < indices.size(); ++i) resultType.width /= original.shape[i];
    resultType.shape.erase(resultType.shape.begin(), resultType.shape.begin() + indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i)
        if (const auto fixed = semantics_.staticIndex(indices[i]); fixed && *fixed >= original.shape[i])
            return invalid(resultType, range);
    std::vector<ValueId> operands;
    operands.reserve(indices.size() + 1);
    operands.push_back(base);
    ValueId valid;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto value = index(indices[i]); operands.push_back(value);
        if (semantics_.staticIndex(indices[i])) continue;
        const auto inside = inBounds(value, original.shape[i], range);
        valid = valid ? op(OperationKind::kAnd, {grh::ValueType::Logic, 1, false}, {valid, inside}, range) : inside;
    }
    const auto result = op(OperationKind::kSliceArray, resultType, operands, range);
    return valid ? op(OperationKind::kMux, resultType, {valid, result, invalid(resultType, range)}, range) : result;
}
ValueId Lowerer::expression(ExprId id, const std::vector<std::string>& fields) {
    const auto& expr = semantics_.body.arena.expressions[id];


    const auto& projectedLeaf = projected(id, fields);
    if (!semantics_.total(projectedLeaf)) return {};
    AccessPath path;
    if (access(id, path)) {
        path.fields.append(fields);
        const auto& driver = findDriver(path.name, path.fields.span());
        const auto base = driver.value;
        if (!base) return invalid(logical(projectedLeaf), expr.range);
        if (!driver.regSymbol.empty() && !path.indices.empty()) return stateRead(driver, path.indices.span(), expr.range);
        return read(base, path.indices.span(), expr.range);
    }
    if (expr.kind == ExprKind::Field) {
        auto projection = fields; projection.insert(projection.begin(), std::string(expr.name));
        return expression(expr.operands[0], projection);
    }
    if (expr.kind == ExprKind::Index) {
        const std::array<ExprId, 1> indexExpression{expr.operands[1]};
        return read(expression(expr.operands[0], fields), indexExpression, expr.range);
    }
    const auto type = logical(projectedLeaf);
    if (expr.kind == ExprKind::UInt || expr.kind == ExprKind::SInt) {
        auto value = literal(expr.literal, type.isSigned, expr.range);
        if (value.negative) value.bits.insert(value.bits.begin(), type.width - value.bits.size(), '1');
        return constant(type, std::move(value.bits), expr.range);
    }
    if (expr.kind == ExprKind::Mux) {
        const auto select = adapt(expression(expr.operands[0]), {grh::ValueType::Logic, 1, false}, expr.range);
        const auto a = adapt(expression(expr.operands[1], fields), type, expr.range);
        const auto b = adapt(expression(expr.operands[2], fields), type, expr.range);
        return op(OperationKind::kMux, type, {select, a, b}, expr.range);
    }
    if (expr.kind == ExprKind::Primitive) {
        return primitive(id, type);
    }
    if (expr.kind == ExprKind::Probe || expr.kind == ExprKind::RWProbe || expr.kind == ExprKind::Read) return expression(expr.operands[0], fields);
    throw Error(expr.range, "FIRRTL expression lowering is not supported yet (F06)");
}

ValueId Lowerer::primitive(ExprId id, const LogicalType& type) {
    const auto& expr = semantics_.body.arena.expressions[id];

    const auto word = expr.name; const auto range = expr.range;
    slang::SmallVector<ValueId, 4> args;
    slang::SmallVector<int64_t, 4> widths, parameters;
    args.reserve(expr.operands.size());
    widths.reserve(expr.operands.size());
    parameters.reserve(expr.operands.size());
    for (const auto arg : expr.operands) {
        if (semantics_.body.arena.expressions[arg].kind == ExprKind::Integer) {
            parameters.push_back(*semantics_.staticIndex(arg));
        } else {
            args.push_back(expression(arg)); widths.push_back(semantics_.width(semantics_.expressions[arg]));
        }
    }
    if (word == "cat") {
        slang::SmallVector<ValueId, 4> nonzero;
        nonzero.reserve(args.size());
        for (const auto arg : args) if (arg) nonzero.push_back(arg);
        return nonzero.size() == 1 ? view(nonzero[0], type, range) :
            op(OperationKind::kConcat, type, std::span<const ValueId>(nonzero.data(), nonzero.size()), range);
    }
    if (word == "asUInt" || word == "asSInt" || word == "asClock" || word == "asReset" || word == "asAsyncReset" || word == "cvt" || word == "pad")
        return adapt(args[0], type, range);
    if (word == "head" || word == "tail" || word == "bits" || word == "shr") {
        const auto start = word == "head" ? widths[0] - parameters[0] : word == "tail" ? 0 :
                           word == "bits" ? parameters[1] : std::min(parameters[0], std::max<int64_t>(0, widths[0] - 1));
        return args[0] ? slice(args[0], start, type.width, type.isSigned, range) : constant(type, "0", range);
    }
    if (word == "shl" || word == "dshl" || word == "dshr") {
        const auto input = adapt(args[0], type, range);
        const auto amount = word == "shl" ? number(parameters[0], 32, range) : args[1] ? args[1] : number(0, 1, range);
        return op(word != "dshr" ? OperationKind::kShl : type.isSigned ? OperationKind::kAShr : OperationKind::kLShr, type, {input, amount}, range);
    }
    if (word == "neg") return op(OperationKind::kSub, type, {constant(type, "0", range), adapt(args[0], type, range)}, range);
    if (word == "not") return op(OperationKind::kNot, type, {adapt(args[0], type, range)}, range);
    if (word == "andr" || word == "orr" || word == "xorr") {
        if (!args[0]) return constant(type, word == "andr" ? "1" : "0", range);
        return op(word == "andr" ? OperationKind::kReduceAnd : word == "orr" ? OperationKind::kReduceOr : OperationKind::kReduceXor, type, {args[0]}, range);
    }
    if (word == "mul") {
        if (!args[0] || !args[1]) return constant(type, "0", range);
        return op(OperationKind::kMul, type, std::span<const ValueId>(args.data(), args.size()), range);
    }
    static const std::map<std::string_view, OperationKind> kinds{
        {"add", OperationKind::kAdd}, {"sub", OperationKind::kSub}, {"div", OperationKind::kDiv}, {"rem", OperationKind::kMod},
        {"eq", OperationKind::kEq}, {"neq", OperationKind::kNe}, {"lt", OperationKind::kLt}, {"leq", OperationKind::kLe},
        {"gt", OperationKind::kGt}, {"geq", OperationKind::kGe}, {"and", OperationKind::kAnd}, {"or", OperationKind::kOr}, {"xor", OperationKind::kXor}};
    const bool compare = word == "eq" || word == "neq" || word == "lt" || word == "leq" || word == "gt" || word == "geq";
    const bool divide = word == "div" || word == "rem";
    auto operandType = type;
    if (compare || divide) operandType.width = static_cast<int32_t>(std::max<int64_t>({1, widths[0], widths[1], divide ? type.width : 1}));
    // Extend with the source's signedness before interpreting a bitwise result as UInt.
    operandType.isSigned = semantics_.expressions[expr.operands[0]]->kind == TypeKind::SInt;
    const auto a = adapt(args[0], operandType, range), b = adapt(args[1], operandType, range);
    auto resultType = divide ? operandType : type;
    auto result = adapt(op(kinds.at(word), resultType, {a, b}, range), type, range);
    if (divide) {
        const auto nonzero = op(OperationKind::kReduceOr, {grh::ValueType::Logic, 1, false}, {b}, range);
        result = op(OperationKind::kMux, type, {nonzero, result, invalid(type, range)}, range);
    }
    return result;
}

ValueId Lowerer::stateRead(const Driver& driver, std::span<const ExprId> indices, Range range) {
    auto type = graph_.valueLogicalType(driver.value);
    const auto shape = type.shape;
    for (std::size_t i = 0; i < indices.size(); ++i) type.width /= shape[i];
    type.shape.erase(type.shape.begin(), type.shape.begin() + indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i)
        if (const auto fixed = semantics_.staticIndex(indices[i]); fixed && *fixed >= shape[i])
            return invalid(type, range);
    std::vector<ValueId> args;
    args.reserve(indices.size());
    ValueId valid;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto value = index(indices[i]); args.push_back(value);
        if (semantics_.staticIndex(indices[i])) continue;
        const auto inside = inBounds(value, shape[i], range);
        valid = valid ? op(OperationKind::kAnd, {grh::ValueType::Logic, 1, false}, {valid, inside}, range) : inside;
    }
    const auto result = op(OperationKind::kRegisterReadArrayPort, type, args, range);
    graph_.setAttr(graph_.valueDef(result), "regSymbol", driver.regSymbol);
    return valid ? op(OperationKind::kMux, type, {valid, result, invalid(type, range)}, range) : result;
}
void Lowerer::stateWrite(const Driver& driver, const Declaration::Register& state, ValueId enable,
                         const std::vector<ValueId>& indices, ValueId data, Range range) {
    const auto writer = graph_.createTemporaryOperation(indices.empty() ? OperationKind::kRegisterWritePort : OperationKind::kRegisterWriteArrayPort);
    graph_.addOperand(writer, enable);
    for (const auto value : indices) graph_.addOperand(writer, value);
    graph_.addOperand(writer, data); graph_.addOperand(writer, expression(state.clock));
    std::vector<std::string> edges{"posedge"};
    if (state.reset && semantics_.resolvedKind(semantics_.expressions[*state.reset]) == TypeKind::AsyncReset) {
        graph_.addOperand(writer, expression(*state.reset)); edges.push_back("posedge");
    }
    graph_.setAttr(writer, "regSymbol", driver.regSymbol); graph_.setAttr(writer, "eventEdge", edges);
    graph_.assignWriterOrder(writer, driver.regSymbol);
    graph_.setOpSrcLocView(writer, location(range));
    setOpSemantic(writer, driver.semantic);
}

ValueId Lowerer::guard(int id, Range range) {
    if (const auto found = guards_.find(id); found != guards_.end()) return found->second;
    SemanticScope scope(currentSemantic_, generatedSemantic("guard_" + std::to_string(id)));
    ValueId result;
    const LogicalType bit{grh::ValueType::Logic, 1, false};
    if (id < 2) result = number(id, 1, range);
    else {
        const auto branch = semantics_.guards.branches[id];
        const auto condition = adapt(expression(semantics_.guards.expressions[branch.variable]), bit, range);
        result = op(OperationKind::kMux, bit, {condition, guard(branch.high, range), guard(branch.low, range)}, range);
    }
    guards_.emplace(id, result); return result;
}
void Lowerer::cover(Driver& driver, int64_t begin, int64_t end, int guard) {
    if (begin == end) return;
    const auto split = [&](int64_t position) {
        auto next = driver.coverage.upper_bound(position);
        const int value = std::prev(next)->second;
        return driver.coverage.try_emplace(position, value).first;
    };
    const auto first = split(begin), last = split(end);
    for (auto at = first; at != last; ++at) at->second = semantics_.guards.disjunction(at->second, guard);
}
void Lowerer::connect(const Connection& connection) {
    if (!connection.guard) return;
    AccessPath path;
    if (!access(connection.sink.expression, path)) throw Error(connection.range, "connect requires a component destination");
    path.fields.append(connection.sink.fields);
    auto& driver = findDriver(path.name, path.fields.span());
    if (!driver.value) return;
    SemanticScope scope(currentSemantic_, driver.semantic);
    const auto fullType = graph_.valueLogicalType(driver.value);
    auto dataType = fullType;
    for (std::size_t i = 0; i < path.indices.size(); ++i) dataType.width /= fullType.shape[i];
    dataType.shape.erase(dataType.shape.begin(), dataType.shape.begin() + path.indices.size());
    for (std::size_t i = 0; i < path.indices.size(); ++i)
        if (const auto fixed = semantics_.staticIndex(path.indices[i]); fixed && *fixed >= fullType.shape[i]) {
            if (connection.define) throw Error(connection.range, "probe definition requires a static component destination");
            return;
        }
    auto data = connection.invalid ? invalid(dataType, connection.range) : adapt(expression(connection.source.expression, connection.source.fields), dataType, connection.range);
    if (!driver.regSymbol.empty()) {
        const auto& state = *semantics_.declaration(path.name).state;
        auto enable = guard(connection.guard, connection.range);
        const LogicalType bit{grh::ValueType::Logic, 1, false};
        if (state.reset) {
            const auto reset = op(OperationKind::kNot, bit, {expression(*state.reset)}, connection.range);
            enable = op(OperationKind::kAnd, bit, {enable, reset}, connection.range);
        }
        std::vector<ValueId> indices;
        indices.reserve(path.indices.size());
        for (std::size_t i = 0; i < path.indices.size(); ++i) {
            const auto value = index(path.indices[i]); indices.push_back(value);
            if (semantics_.staticIndex(path.indices[i])) continue;
            enable = op(OperationKind::kAnd, bit, {enable, inBounds(value, fullType.shape[i], connection.range)}, connection.range);
        }
        stateWrite(driver, state, enable, indices, data, connection.range);
        return;
    }
    int64_t offset = 0, stride = fullType.width; bool staticWrite = true;
    for (std::size_t i = 0; i < path.indices.size(); ++i) {
        stride /= fullType.shape[i]; const auto fixed = semantics_.staticIndex(path.indices[i]);
        if (!fixed || *fixed >= fullType.shape[i]) staticWrite = false;
        else offset += *fixed * stride;
    }
    if (connection.define) {
        if (!staticWrite) throw Error(connection.range, "probe definition requires a static component destination");
        for (auto at = driver.coverage.begin(); std::next(at) != driver.coverage.end(); ++at)
            if (at->first < offset + dataType.width && std::next(at)->first > offset && at->second)
                throw Error(connection.range, "probe has multiple definitions");
    }
    if (staticWrite) cover(driver, offset, offset + dataType.width, connection.guard);

    auto flatType = fullType; flatType.shape.clear(); flatType.isSigned = false;
    auto materializePieces = [&]() {
        if (!driver.pieces) return;
        std::vector<ValueId> parts;
        parts.reserve(driver.pieces->size());
        for (auto it = driver.pieces->rbegin(); it != driver.pieces->rend(); ++it)
            parts.push_back(it->second.value);
        const auto packed = parts.size() == 1 ? parts.front() : op(OperationKind::kConcat, flatType, parts, connection.range);
        driver.current = view(packed, fullType, connection.range);
    };
    auto splitPiece = [&](int64_t position) {
        if (position <= 0 || position >= fullType.width || !driver.pieces) return;
        auto next = driver.pieces->upper_bound(position);
        if (next == driver.pieces->begin()) return;
        auto piece = std::prev(next);
        if (position == piece->first || position >= piece->second.end) return;
        const auto begin = piece->first;
        const auto end = piece->second.end;
        const auto value = piece->second.value;
        const auto lower = slice(value, 0, position - begin, false, connection.range);
        const auto upper = slice(value, position - begin, end - position, false, connection.range);
        piece->second.end = position;
        piece->second.value = lower;
        driver.pieces->emplace(position, DriverPiece{end, upper});
    };
    auto ensurePieces = [&]() {
        if (driver.pieces) return;
        const auto base = driver.current ? view(driver.current, flatType, connection.range) : invalid(flatType, connection.range);
        driver.pieces = std::make_unique<DriverPieces>();
        driver.pieces->emplace(0, DriverPiece{fullType.width, base});
    };
    auto replacePieceRange = [&](ValueId value) {
        ensurePieces();
        splitPiece(offset); splitPiece(offset + dataType.width);
        auto first = driver.pieces->lower_bound(offset);
        auto last = driver.pieces->lower_bound(offset + dataType.width);
        driver.pieces->erase(first, last);
        driver.pieces->emplace(offset, DriverPiece{offset + dataType.width, value});
        materializePieces();
    };
    if (!path.indices.empty() && !staticWrite && driver.pieces) {
        materializePieces();
        driver.pieces.reset();
    }
    auto current = driver.current;
    if (!current) current = invalid(fullType, connection.range);
    auto enable = guard(connection.guard, connection.range);
    const bool trackMemoryMask = semantics_.memoryPorts.contains(path.name);
    ValueId writeMask;
    if (trackMemoryMask) writeMask = constant(fullType, std::string(dataType.width, '1'), connection.range);
    if (!path.indices.empty() && staticWrite) {
        // Guard only the written slice. An array-wide mux would introduce
        // false dependencies from the condition to untouched elements.
        auto partType = dataType; partType.shape.clear(); partType.isSigned = false;
        ensurePieces();
        auto part = view(data, partType, connection.range);
        if (connection.guard != 1) {
            // Keep the old arm local to the written range, without retaining a whole-array snapshot.
            splitPiece(offset); splitPiece(offset + dataType.width);
            const auto first = driver.pieces->lower_bound(offset);
            auto last = driver.pieces->lower_bound(offset + dataType.width);
            std::vector<ValueId> priorParts;
            while (last != first) priorParts.push_back((--last)->second.value);
            const auto prior = priorParts.size() == 1 ? view(priorParts.front(), partType, connection.range) :
                op(OperationKind::kConcat, partType, priorParts, connection.range);
            part = op(OperationKind::kMux, partType,
                {enable, part, prior}, connection.range);
        }
        replacePieceRange(part);
        data = driver.current;
        if (trackMemoryMask)
            writeMask = constant(fullType, std::string(dataType.width, '1') + std::string(offset, '0'), connection.range);
    } else if (!path.indices.empty()) {
        const LogicalType offsetType{grh::ValueType::Logic, 32, false};
        const LogicalType bit{grh::ValueType::Logic, 1, false};
        auto bitOffset = number(0, 32, connection.range); stride = fullType.width;
        for (std::size_t i = 0; i < path.indices.size(); ++i) {
            stride /= fullType.shape[i]; const auto value = index(path.indices[i]);
            if (!semantics_.staticIndex(path.indices[i]))
                enable = op(OperationKind::kAnd, bit, {enable, inBounds(value, fullType.shape[i], connection.range)}, connection.range);
            auto term = adapt(value, offsetType, connection.range);
            if (stride != 1) {
                auto productType = offsetType; productType.width = 64;
                term = adapt(op(OperationKind::kMul, productType, {term, number(stride, 32, connection.range)}, connection.range), offsetType, connection.range);
            }
            bitOffset = op(OperationKind::kAdd, offsetType, {bitOffset, term}, connection.range);
        }
        auto flatType = fullType; flatType.shape.clear(); flatType.isSigned = false;
        auto partType = dataType; partType.shape.clear(); partType.isSigned = false;
        auto payload = adapt(view(data, partType, connection.range), flatType, connection.range);
        const auto mask = constant(flatType, std::string(dataType.width, '1'), connection.range);
        payload = op(OperationKind::kShl, flatType, {payload, bitOffset}, connection.range);
        const auto shifted = op(OperationKind::kShl, flatType, {mask, bitOffset}, connection.range);
        if (trackMemoryMask) writeMask = view(shifted, fullType, connection.range);
        const auto clear = op(OperationKind::kNot, flatType, {shifted}, connection.range);
        const auto kept = op(OperationKind::kAnd, flatType, {view(current, flatType, connection.range), clear}, connection.range);
        data = view(op(OperationKind::kOr, flatType, {kept, payload}, connection.range), fullType, connection.range);
    }
    if (!(staticWrite && !path.indices.empty())) {
        driver.current = staticWrite && connection.guard == 1 ? data :
            op(OperationKind::kMux, fullType, {enable, data, view(current, fullType, connection.range)}, connection.range);
        driver.pieces = std::make_unique<DriverPieces>();
        driver.pieces->emplace(0, DriverPiece{fullType.width, view(driver.current, flatType, connection.range)});
    }
    if (trackMemoryMask) {
        auto mask = driver.memoryMask ? driver.memoryMask : number(0, fullType.width, connection.range);
        mask = view(mask, fullType, connection.range);
        const auto merged = op(OperationKind::kOr, fullType, {mask, writeMask}, connection.range);
        driver.memoryMask = op(OperationKind::kMux, fullType, {enable, merged, mask}, connection.range);
    }
}
void Lowerer::drive(ValueId target, ValueId source, Range range, bool root) {
    if (!target) return;
    const auto operation = graph_.createTemporaryOperation(OperationKind::kAssign);
    graph_.addOperand(operation, source); graph_.addResult(operation, target); graph_.setOpSrcLocView(operation, location(range));
    setOpSemantic(operation);
    if (root && currentSemantic_) {
        const std::span<const grh::SemanticId> semantic(&currentSemantic_, 1);
        graph_.setValueSemanticIds(target, semantic);
    }
}

bool Lowerer::memorySource(const LeafKey& key) const {
    const auto found = semantics_.memories.find(key.first);
    if (found == semantics_.memories.end() || !found->second.standard || key.second.size() < 2) return false;
    for (const auto& port : found->second.ports)
        if (port.name == key.second[0] && port.read && key.second[1] == (port.write ? "rdata" : "data")) return true;
    return false;
}
ValueId Lowerer::pipeline(ValueId data, ValueId clock, int64_t stages, Range range) {
    const auto type = graph_.valueLogicalType(data);
    for (int64_t i = 0; i < stages; ++i) {
        const auto state = graph_.createOperation(OperationKind::kRegister);
        const std::string symbol(graph_.getOperation(state).symbolText());
        grh::setStateLogicalType(graph_, state, type);
        grh::InitStep init; init.value = std::to_string(type.width) + (options_.initPolicy == FirrtlInitPolicy::Unknown ? "'bx" : "'b0");
        grh::setStateInitSpec(graph_, state, {init}); graph_.setOpSrcLocView(state, location(range));
        graph_.setAttr(state, "firrtl.memoryPipeline", true);
        setOpSemantic(state);
        const auto write = graph_.createTemporaryOperation(OperationKind::kRegisterWritePort);
        for (const auto value : {number(1, 1, range), data, clock}) graph_.addOperand(write, value);
        graph_.setAttr(write, "regSymbol", symbol); graph_.setAttr(write, "eventEdge", std::vector<std::string>{"posedge"});
        graph_.assignWriterOrder(write, symbol);
        graph_.setOpSrcLocView(write, location(range));
        setOpSemantic(write);
        data = op(OperationKind::kRegisterReadPort, type, {}, range); graph_.setAttr(graph_.valueDef(data), "regSymbol", symbol);
    }
    return data;
}
void Lowerer::memories() {
    const LogicalType bit{grh::ValueType::Logic, 1, false};
    const auto both = [&](ValueId a, ValueId b, Range r) { return op(OperationKind::kAnd, bit, {a, b}, r); };
    std::unordered_map<ValueId, ValueId, grh::ValueIdHash> aliases;
    for (const auto& [key, driver] : drivers_)
        if (driver.value && driver.current) aliases.emplace(driver.value, driver.current);
    const auto clockSource = [&](ValueId clock) {
        std::unordered_set<ValueId, grh::ValueIdHash> visited;
        while (visited.insert(clock).second) {
            if (const auto found = aliases.find(clock); found != aliases.end()) { clock = found->second; continue; }
            const auto definition = graph_.valueDef(clock);
            if (!definition || graph_.opKind(definition) != OperationKind::kAssign) break;
            const auto operands = graph_.opOperands(definition);
            if (operands.size() != 1 || graph_.valueWidth(operands[0]) != 1) break;
            clock = operands[0];
        }
        return clock;
    };
    for (const auto& [name, memory] : semantics_.memories) {
        if (memory.readUnderWrite == "undefined") {
            std::vector<std::string> warnings;
            if (const auto existing = graph_.metadata("firrtl.readUnderWriteWarnings"))
                warnings = std::get<std::vector<std::string>>(*existing);
            warnings.push_back("memory '" + name + "' read-under-write=undefined uses new-value collision behavior and ungated disabled/OOB reads for two-state simulation");
            graph_.setMetadata("firrtl.readUnderWriteWarnings", warnings);
        }
        std::map<std::string, ValueId> writeModes;
        if (!memory.standard) for (const auto& port : memory.ports) if (port.read && port.write) {
            std::vector<grh::SemanticId> semantics;
            for (const auto& leaf : semantics_.leavesCached(memory.data)) {
                const auto semantic = findDriver(port.name, leaf.fields).semantic;
                if (semantic) semantics.push_back(semantic);
            }
            const auto operationBegin = graph_.operations().size();
            auto mode = number(0, 1, port.range);
            for (const auto& leaf : semantics_.leavesCached(memory.data)) {
                const auto mask = findDriver(port.name, leaf.fields).memoryMask;
                if (mask) mode = op(OperationKind::kOr, bit, {mode, op(OperationKind::kReduceOr, bit, {mask}, port.range)}, port.range);
            }
            const auto operations = graph_.operations();
            for (std::size_t i = operationBegin; i < operations.size(); ++i)
                if (!semantics.empty() && graph_.opSemanticIds(operations[i]).empty())
                    graph_.setOpSemanticIds(operations[i], semantics);
            writeModes.emplace(port.name, mode);
        }
        struct Port {
            const MemoryPort* source; grh::SemanticId semantic = 0;
            ValueId address, clock, readEnable, writeEnable;
            ValueId writeAddress, writeData, writeMask;
        };
        for (const auto& leaf : semantics_.leavesCached(memory.data)) {
            const auto type = logical(leaf); if (!type.width) continue;
            auto storage = type; storage.shape.insert(storage.shape.begin(), memory.depth);
            auto storedLeaf = leaf; storedLeaf.shape.insert(storedLeaf.shape.begin(), memory.depth);
            storage.width = static_cast<int32_t>(semantics_.total(storedLeaf));
            grh::SemanticId storageSemantic = 0;
            if (auto* semanticTree = graph_.semanticTree()) {
                auto [it, inserted] = semanticDeclarations_.try_emplace(name);
                if (inserted)
                    it->second = semanticTree->addNode(semanticTree->root(), grh::SemanticNodeKind::Declaration, name);
                storageSemantic = memory.standard ? it->second : semanticTree->addFieldPath(it->second, leaf.fields);
            }
            const auto state = graph_.createOperation(OperationKind::kRegister);
            const std::string symbol(graph_.getOperation(state).symbolText());
            grh::setStateLogicalType(graph_, state, storage);
            grh::InitStep init; init.value = std::to_string(storage.width) + (options_.initPolicy == FirrtlInitPolicy::Unknown ? "'bx" : "'b0");
            grh::setStateInitSpec(graph_, state, {init}); graph_.setOpSrcLocView(state, location(memory.range));
            graph_.setAttr(state, "firrtl.memory", name); graph_.setAttr(state, "firrtl.readUnderWrite", memory.readUnderWrite);
            setOpSemantic(state, storageSemantic);
            std::vector<Port> ports;
            for (const auto& port : memory.ports) {
                const auto field = [&](std::string_view member, bool data = false) -> Driver& {
                    std::vector<std::string> path{port.name, std::string(member)};
                    if (data) path.insert(path.end(), leaf.fields.begin(), leaf.fields.end());
                    return findDriver(name, path);
                };
                Port lowered{&port};
                if (memory.standard) {
                    std::vector<std::string> semanticPath{port.name, port.read ? (port.write ? "rdata" : "data") : "data"};
                    semanticPath.insert(semanticPath.end(), leaf.fields.begin(), leaf.fields.end());
                    lowered.semantic = findDriver(name, semanticPath).semantic;
                } else {
                    lowered.semantic = findDriver(port.name, leaf.fields).semantic;
                }
                SemanticScope scope(currentSemantic_, lowered.semantic);
                if (memory.standard) {
                    auto address = field("addr").value;
                    lowered.address = address ? address : number(0, 1, port.range);
                    lowered.clock = clockSource(field("clk").current ? field("clk").current : field("clk").value);
                    lowered.readEnable = lowered.writeEnable = field("en").value;
                    if (port.read && port.write) {
                        const auto mode = field("wmode").value;
                        lowered.readEnable = both(lowered.readEnable, op(OperationKind::kNot, bit, {mode}, port.range), port.range);
                        lowered.writeEnable = both(lowered.writeEnable, mode, port.range);
                    }
                    if (port.write) {
                        lowered.writeData = field(port.read ? "wdata" : "data", true).value;
                        const auto mask = field(port.read ? "wmask" : "mask", true).value;
                        const auto& maskType = graph_.valueLogicalType(mask);
                        if (type.shape.empty()) {
                            lowered.writeMask = type.width == 1 ? view(mask, type, port.range) : op(OperationKind::kReplicate, type, {mask}, port.range);
                            if (type.width != 1) graph_.setAttr(graph_.valueDef(lowered.writeMask), "rep", int64_t(type.width));
                        } else {
                            // Repeat each mask leaf's bit across its corresponding data element.
                            std::vector<ValueId> elements; const auto count = maskType.width;
                            for (int64_t i = 0; i < count; ++i) {
                                const auto selected = slice(mask, i, 1, false, port.range);
                                const auto width = type.width / count;
                                auto expanded = selected;
                                if (width > 1) { expanded = op(OperationKind::kReplicate, {grh::ValueType::Logic, width, false}, {selected}, port.range); graph_.setAttr(graph_.valueDef(expanded), "rep", width); }
                                elements.insert(elements.begin(), expanded);
                            }
                            auto flat = type; flat.shape.clear(); flat.isSigned = false;
                            lowered.writeMask = view(elements.size() == 1 ? elements[0] : op(OperationKind::kConcat, flat, elements, port.range), type, port.range);
                        }
                    }
                } else {
                    lowered.address = index(*port.address); lowered.clock = clockSource(expression(*port.clock));
                    lowered.readEnable = guard(port.guard, port.range);
                    auto& driver = findDriver(port.name, leaf.fields);
                    lowered.writeData = driver.current ? view(driver.current, type, port.range) : invalid(type, port.range);
                    lowered.writeMask = driver.memoryMask ? driver.memoryMask : view(number(0, type.width, port.range), type, port.range);
                    lowered.writeEnable = both(guard(port.guard, port.range), op(OperationKind::kReduceOr, bit, {lowered.writeMask}, port.range), port.range);
                    if (port.read && port.write) lowered.readEnable = both(lowered.readEnable, op(OperationKind::kNot, bit, {writeModes.at(port.name)}, port.range), port.range);
                }
                if (port.write) {
                    lowered.writeEnable = both(lowered.writeEnable, op(OperationKind::kReduceOr, bit, {lowered.writeMask}, port.range), port.range);
                    lowered.writeAddress = lowered.address;
                    for (int64_t i = 1; i < memory.writeLatency; ++i) {
                        lowered.writeAddress = pipeline(lowered.writeAddress, lowered.clock, 1, port.range);
                        lowered.writeEnable = pipeline(lowered.writeEnable, lowered.clock, 1, port.range);
                    }
                    lowered.writeData = pipeline(lowered.writeData, lowered.clock, memory.writeLatency - 1, port.range);
                    lowered.writeMask = pipeline(lowered.writeMask, lowered.clock, memory.writeLatency - 1, port.range);
                }
                ports.push_back(lowered);
            }
            for (const auto& port : ports) if (port.source->write) {
                SemanticScope scope(currentSemantic_, port.semantic);
                const auto writer = graph_.createTemporaryOperation(OperationKind::kRegisterWriteArrayMaskPort);
                for (const auto value : {port.writeEnable, port.writeAddress, port.writeData, port.writeMask, port.clock}) graph_.addOperand(writer, value);
                graph_.setAttr(writer, "regSymbol", symbol); graph_.setAttr(writer, "eventEdge", std::vector<std::string>{"posedge"});
                graph_.assignWriterOrder(writer, symbol);
                graph_.setOpSrcLocView(writer, location(port.source->range));
                setOpSemantic(writer);
            }
            for (const auto& port : ports) {
                SemanticScope scope(currentSemantic_, port.semantic);
                ValueId data;
                if (port.source->read) {
                    auto address = port.address, enable = port.readEnable;
                    const bool delayAddress = memory.readUnderWrite != "old";
                    const bool gateRead = memory.readUnderWrite != "undefined";
                    if (delayAddress) {
                        address = pipeline(address, port.clock, memory.readLatency, port.source->range);
                        if (gateRead) enable = pipeline(enable, port.clock, memory.readLatency, port.source->range);
                    }
                    data = op(OperationKind::kRegisterReadArrayPort, type, {address}, port.source->range);
                    graph_.setAttr(graph_.valueDef(data), "regSymbol", symbol);
                    if (gateRead) data = op(OperationKind::kMux, type, {enable, data, invalid(type, port.source->range)}, port.source->range);
                    if (!delayAddress) data = pipeline(data, port.clock, memory.readLatency, port.source->range);
                } else data = invalid(type, port.source->range);
                std::vector<std::string> fields;
                std::string target = port.source->name;
                if (memory.standard) {
                    if (!port.source->read) continue;
                    target = name; fields = {port.source->name, port.source->write ? "rdata" : "data"};
                }
                fields.insert(fields.end(), leaf.fields.begin(), leaf.fields.end());
                // Write-only mports have no expression value. Their data is
                // consumed directly from the ordered connection driver.
                if (memory.standard || port.source->read) drive(findDriver(target, fields).value, data, port.source->range);
            }
        }
    }
}
void Lowerer::effects() {
    const LogicalType bit{grh::ValueType::Logic, 1, false};
    std::size_t effectIndex = 0;
    for (const auto& effect : semantics_.effects) {
        const auto& statement = semantics_.body.arena.statements[effect.statement];
        SemanticScope scope(currentSemantic_, generatedSemantic(
            "effect@" + std::to_string(statement.range.line) + ":" + std::to_string(statement.range.column) +
            "#" + std::to_string(effectIndex++)));
        auto operands = statement.operands;
        std::string text, name; std::vector<ValueId> arguments;
        const bool verification = statement.kind == StmtKind::Assert || statement.kind == StmtKind::Assume || statement.kind == StmtKind::Cover || statement.kind == StmtKind::Intrinsic;
        if (statement.kind == StmtKind::Intrinsic) {
            const auto& call = semantics_.body.arena.expressions[operands[0]]; operands = call.operands;
            for (const auto& parameter : call.parameters) if (parameter.name == "format") text = decodeString(parameter.value);
            for (std::size_t i = 3; i < operands.size(); ++i) arguments.push_back(expression(operands[i]));
        } else if (statement.kind != StmtKind::Stop) {
            const auto format = verification ? 3u : 2u;
            text = decodeString(semantics_.body.arena.expressions[operands[format]].literal);
            for (std::size_t i = format + 1; i < operands.size(); ++i) arguments.push_back(expression(operands[i]));
        }
        const auto clock = expression(operands[0]);
        auto enable = expression(operands[verification ? 2 : 1]);
        enable = op(OperationKind::kAnd, bit, {enable, guard(effect.guard, statement.range)}, statement.range);
        if (verification) {
            auto predicate = expression(operands[1]);
            if (statement.kind != StmtKind::Cover) predicate = op(OperationKind::kNot, bit, {predicate}, statement.range);
            enable = op(OperationKind::kAnd, bit, {enable, predicate}, statement.range);
        }
        if (statement.kind == StmtKind::Stop) {
            name = "finish";
            const auto& code = semantics_.body.arena.expressions[operands[2]];
            arguments.push_back(number(integer64({TokenKind::Number, code.literal, code.range}), 32, code.range));
        } else {
            // FIRRTL %x is the same base as the existing SV formatter's %h.
            // Cover carries a literal comment, not a printf format string.
            if (statement.kind == StmtKind::Cover) {
                std::string escaped;
                for (const char c : text) { escaped += c; if (c == '%') escaped += '%'; }
                text = std::move(escaped);
            }
            std::size_t placeholders = 0;
            for (std::size_t i = 0; i < text.size(); ++i) if (text[i] == '%') {
                if (++i == text.size()) throw Error(statement.range, "truncated FIRRTL format placeholder");
                if (text[i] == '%') continue;
                if (std::string_view("bcdx").find(text[i]) == std::string_view::npos) throw Error(statement.range, "unsupported FIRRTL format placeholder");
                ++placeholders; if (text[i] == 'x') text[i] = 'h';
            }
            if (statement.kind != StmtKind::Cover && text.find("{{") != std::string::npos) throw Error(statement.range, "FIRRTL special format substitution is not supported yet");
            if (statement.kind != StmtKind::Cover && placeholders != arguments.size()) throw Error(statement.range, "FIRRTL format argument count mismatch");
            const auto constant = graph_.createTemporaryOperation(OperationKind::kConstant);
            const auto value = graph_.createTemporaryValue({grh::ValueType::String, 0, false});
            graph_.addResult(constant, value); graph_.setAttr(constant, "constValue", text); graph_.setOpSrcLocView(constant, location(statement.range));
            setOpSemantic(constant);
            arguments.insert(arguments.begin(), value);
            name = verification && statement.kind != StmtKind::Cover ? "fatal" : "write";
            if (name == "fatal") arguments.insert(arguments.begin(), number(1, 32, statement.range));
        }
        const auto task = graph_.createOperation(OperationKind::kSystemTask);
        graph_.addOperand(task, enable); for (const auto value : arguments) graph_.addOperand(task, value); graph_.addOperand(task, clock);
        graph_.setAttr(task, "name", name); graph_.setAttr(task, "eventEdge", std::vector<std::string>{"posedge"});
        graph_.setAttr(task, "procKind", std::string("always")); graph_.setAttr(task, "hasTiming", true);
        graph_.setAttr(task, "firrtl.effectKind", std::string(statement.kind == StmtKind::Cover ? "cover-hit" : verification ? "assert" : statement.kind == StmtKind::Stop ? "stop" : "printf"));
        graph_.setAttr(task, "firrtl.effectLabel", std::string(statement.name)); graph_.setOpSrcLocView(task, location(statement.range));
        // Keep the originating FIRRTL statement line as a stable cross-tool
        // identity. GSIM uses this same parser line for ASSERT_/PRINTF_/STOP_
        // special-node names and its serialized lineno field.
        graph_.setAttr(task, "firrtl.line", static_cast<int64_t>(statement.range.line));
        setOpSemantic(task);
    }
}
void Lowerer::checkCycles() const {
    std::unordered_map<ValueId, unsigned, grh::ValueIdHash> colors;
    // Iterative DFS avoids host-stack growth on long chains of named nodes.
    struct Frame { ValueId value; std::size_t next = 0; };
    for (const auto value : graph_.values()) {
        if (colors[value] == 2) continue;
        std::vector<Frame> stack{{value}}; colors[value] = 1;
        while (!stack.empty()) {
            auto& frame = stack.back(); const auto definition = graph_.valueDef(frame.value);
            if (!definition || graph_.opKind(definition) == OperationKind::kInstance || frame.next == graph_.opOperands(definition).size()) {
                colors[frame.value] = 2; stack.pop_back(); continue;
            }
            const auto dependency = graph_.opOperands(definition)[frame.next++];
            if (colors[dependency] == 1) {
                const auto begin = std::find_if(stack.begin(), stack.end(), [&](const auto& entry) { return entry.value == dependency; });
                const bool arrayCycle = std::any_of(begin, stack.end(), [&](const auto& entry) {
                    return !graph_.valueShape(entry.value).empty();
                });
                if (arrayCycle) {
                    // Partial connects preserve arrays. A dependency on another
                    // element can look cyclic at whole-value granularity. The
                    // linked frontend must resolve it with range analysis.
                    graph_.setMetadata("firrtl.pendingCombLoopCheck", true);
                    continue;
                }
                const auto loc = graph_.valueSrcLoc(dependency);
                Range range;
                if (loc) {
                    range.line = loc->line; range.column = loc->column;
                    for (const auto& [key, driver] : drivers_)
                        if (driver.value == dependency) { range = driver.range; break; }
                }
                throw Error(range, "combinational cycle in FIRRTL connections");
            }
            if (colors[dependency] != 2) { colors[dependency] = 1; stack.push_back({dependency}); }
        }
    }
}
void Lowerer::instances() {
    for (const auto& instance : semantics_.instances) {
        const auto operation = graph_.createOperation(OperationKind::kInstance);
        graph_.setAttr(operation, "moduleName", instance.target);
        graph_.setAttr(operation, "instanceName", instance.name);
        // External-module/instance boundaries retain the FIRRTL statement
        // line so they can be matched to GSIM's boundary node lineno.
        graph_.setAttr(operation, "firrtl.line", static_cast<int64_t>(instance.range.line));
        graph_.setOpSrcLocView(operation, location(instance.range));
        if (const auto found = semanticDeclarations_.find(instance.name); found != semanticDeclarations_.end())
            setOpSemantic(operation, found->second);
        std::vector<std::string> inputs, outputs;
        const auto& declaration = semantics_.declaration(instance.name);
        for (const auto& leaf : semantics_.leavesCached(declaration.type, declaration.flow)) {
            const auto& driver = findDriver(instance.name, leaf.fields);
            if (!driver.value) continue;
            std::string port = leaf.fields.front();
            for (std::size_t i = 1; i < leaf.fields.size(); ++i) port += "__DOT__" + leaf.fields[i];
            if (leaf.flow == 2) { inputs.push_back(port); graph_.addOperand(operation, driver.value); }
            else { outputs.push_back(port); graph_.addResult(operation, driver.value); }
        }
        graph_.setAttr(operation, "inputPortName", std::move(inputs));
        graph_.setAttr(operation, "outputPortName", std::move(outputs));
        graph_.setAttr(operation, "inoutPortName", std::vector<std::string>{});
    }
}
ModuleSignature Lowerer::run() {
    const auto& module = semantics_.body.interface;
    ModuleSignature signature; signature.language = SourceLanguage::Firrtl;
    signature.name = module.name; signature.sourceUnit = semantics_.source.unit();
    signature.location = semantics_.source.position(module.range);
    graph_.setMetadata("firrtl.version", std::string(semantics_.source.version()));
    graph_.setMetadata("firrtl.sourceUnit", std::string(semantics_.source.unit()));
    graph_.setMetadata("firrtl.invalidPolicy", std::string(options_.invalidPolicy == FirrtlInvalidPolicy::Unknown ? "unknown" : "zero"));
    graph_.setMetadata("firrtl.initPolicy", std::string(options_.initPolicy == FirrtlInitPolicy::Unknown ? "unknown" : "zero"));
    graph_.setMetadata("firrtl.enabledLayers", options_.enabledLayers);
    grh::SemanticTree* semanticTree = nullptr;
    if (options_.nodeAnnotations)
        semanticTree = &graph_.getOrCreateSemanticTree(std::string(semantics_.source.unit()),
            std::string(semantics_.source.circuit()), std::string(module.name));
    std::set<std::string> names; std::vector<std::string> elided;
    rootDrivers_.reserve(semantics_.declarationOrder.size());
    semanticDeclarations_.reserve(semantics_.declarationOrder.size());
    for (const auto& name : semantics_.declarationOrder) {
        const auto& declaration = semantics_.declaration(name);
        if (semanticTree) {
            const auto kind = declaration.definition ? grh::SemanticNodeKind::Expression : grh::SemanticNodeKind::Declaration;
            semanticDeclarations_.emplace(name, semanticTree->addNode(semanticTree->root(), kind, name));
        }
        for (const auto& leaf : semantics_.leavesCached(declaration.type, declaration.flow)) {
            auto symbol = name; for (const auto& field : leaf.fields) symbol += "__DOT__" + field;
            if (!names.insert(symbol).second) throw Error(declaration.range, "flattened FIRRTL name collision '" + symbol + "'");
            const auto type = logical(leaf); ValueId value;
            grh::SemanticId semantic = 0;
            if (semanticTree && type.width)
                semantic = semanticTree->addFieldPath(semanticDeclarations_.at(name), leaf.fields);
            if (type.width) {
                value = declaration.state ? graph_.createTemporaryValue(type) : graph_.createValue(graph_.internSymbol(symbol), type);
                graph_.setValueSrcLocView(value, location(declaration.range));
                if (semantic) graph_.setValueSemanticIds(value, std::span<const grh::SemanticId>(&semantic, 1));
                if (declaration.port) {
                    if (leaf.flow == 1) graph_.bindInputPort(symbol, value);
                    else graph_.bindOutputPort(symbol, value);
                }
            } else elided.push_back(symbol);
            Driver driver{leaf, value, {}, declaration.range, {{0, 0}, {type.width, 0}}};
            driver.semantic = semantic;
            if (declaration.state && value) {
                driver.regSymbol = symbol;
                const auto state = graph_.createOperation(OperationKind::kRegister, graph_.internSymbol(symbol));
                grh::setStateLogicalType(graph_, state, type);
                grh::InitStep init; init.value = std::to_string(type.width) + (options_.initPolicy == FirrtlInitPolicy::Unknown ? "'bx" : "'b0");
                grh::setStateInitSpec(graph_, state, {init}); graph_.setOpSrcLocView(state, location(declaration.range));
                const auto read = graph_.createTemporaryOperation(OperationKind::kRegisterReadPort);
                graph_.addResult(read, value); graph_.setAttr(read, "regSymbol", symbol); graph_.setOpSrcLocView(read, location(declaration.range));
                if (semantic) {
                    graph_.setOpSemanticIds(state, std::span<const grh::SemanticId>(&semantic, 1));
                    graph_.setOpSemanticIds(read, std::span<const grh::SemanticId>(&semantic, 1));
                }
            }
            const auto entry = drivers_.emplace(LeafKey{name, leaf.fields}, std::move(driver)).first;
            if (leaf.fields.empty()) rootDrivers_.emplace(entry->first.first, &entry->second);
            if (declaration.port) {
                PortLeafSignature port; port.name = symbol; port.fieldPath = {name};
                port.fieldPath.insert(port.fieldPath.end(), leaf.fields.begin(), leaf.fields.end());
                port.direction = leaf.flow == 1 ? LeafDirection::Input : LeafDirection::Output;
                port.elementWidth = static_cast<int32_t>(semantics_.width(leaf.type)); port.shape = leaf.shape;
                port.shapeFieldDepths = leaf.shapeFieldDepths;
                port.isSigned = type.isSigned; port.isConst = leaf.type->constant; port.requiresTypeResolution = !type.width;
                const auto kind = semantics_.resolvedKind(leaf.type);
                port.sourceType = kind == TypeKind::SInt ? "SInt" : kind == TypeKind::Clock ? "Clock" : kind == TypeKind::AsyncReset ? "AsyncReset" : "UInt";
                if (leaf.probe != TypeKind::Unknown) {
                    port.sourceType = std::string(leaf.probe == TypeKind::Probe ? "Probe<" : "RWProbe<") + port.sourceType + ">";
                    port.requiresTypeResolution = true;
                }
                signature.ports.push_back(std::move(port));
            }
        }
    }
    if (!elided.empty()) graph_.setMetadata("firrtl.elidedLeaves", std::move(elided));
    for (const auto& name : semantics_.declarationOrder) {
        const auto& declaration = semantics_.declaration(name);
        if (!declaration.definition) continue;
        for (const auto& leaf : semantics_.leavesCached(declaration.type)) {
            auto& driver = findDriver(name, leaf.fields);
            if (!driver.value) continue;
            currentSemantic_ = driver.semantic;
            drive(driver.value, expression(*declaration.definition, leaf.fields), declaration.range, true);
            currentSemantic_ = 0;
        }
    }
    instances();
    for (const auto& connection : semantics_.connections) connect(connection);
    // Reset is a final whole-state writer barrier, independent of declaration
    // placement and ordinary connect priority. Every operand still reads old state.
    for (const auto& name : semantics_.declarationOrder) {
        const auto& declaration = semantics_.declaration(name);
        if (!declaration.state || !declaration.state->reset) continue;
        for (const auto& leaf : semantics_.leavesCached(declaration.type)) {
            const auto& driver = findDriver(name, leaf.fields);
            if (!driver.value) continue;
            SemanticScope scope(currentSemantic_, driver.semantic);
            const auto data = adapt(expression(*declaration.state->initial, leaf.fields), logical(leaf), declaration.range);
            stateWrite(driver, *declaration.state, expression(*declaration.state->reset), {}, data, declaration.range);
        }
    }
    memories();
    effects();
    for (auto& [key, driver] : drivers_) {
        if (!driver.value || !driver.regSymbol.empty() || !(driver.leaf.flow & 2)) continue;
        if (memorySource(key) || semantics_.memoryPorts.contains(key.first)) continue;
        const auto required = semantics_.declaration(key.first).guard;
        for (auto at = driver.coverage.begin(); std::next(at) != driver.coverage.end(); ++at)
            if (semantics_.guards.conjunction(required, semantics_.guards.negate(at->second)))
                throw Error(driver.range, "FIRRTL component '" + key.first + "' is not fully initialized on every path");
        SemanticScope scope(currentSemantic_, driver.semantic);
        drive(driver.value, driver.current ? driver.current : invalid(graph_.valueLogicalType(driver.value), driver.range), driver.range);
    }
    checkCycles();
    if (!graph_.metadata("firrtl.pendingCombLoopCheck")) graph_.markLocalCyclesChecked();
    return signature;
}
} // namespace

ModuleSignature lowerModule(const Source& source, const ModuleBody& body,
                            grh::Design& design, const FirrtlOptions& options,
                            const std::map<std::string, ModuleSignature>* interfaces) {
    if (body.interface.kind != ModuleKind::Module)
        throw Error(body.interface.range, "FIRRTL external/class lowering is not supported yet (F06/F07)");
    Semantics semantics(source, body, options, interfaces); semantics.analyze();
    auto& graph = design.createGraph(std::string(body.interface.name));
    return Lowerer(semantics, graph, options).run();
}

ModuleSignature lowerModule(const Source& source, const ModuleBody& body,
                            grh::Graph& graph, const FirrtlOptions& options,
                            const std::map<std::string, ModuleSignature>* interfaces) {
    if (body.interface.kind != ModuleKind::Module)
        throw Error(body.interface.range, "FIRRTL external/class lowering is not supported yet (F06/F07)");
    Semantics semantics(source, body, options, interfaces); semantics.analyze();
    return Lowerer(semantics, graph, options).run();
}

ModuleSignature lowerModule(Semantics& semantics, grh::Graph& graph,
                            const FirrtlOptions& options) {
    if (!semantics.analyzed()) throw std::logic_error("FIRRTL lowering requires analyzed semantics");
    return Lowerer(semantics, graph, options).run();
}
} // namespace wolvrix::lib::ingest::firrtl::detail
