#include "semantic.hpp"
#include "ingest/node_attributes.hpp"

#include <algorithm>
#include <bit>
#include <set>

namespace wolvrix::lib::ingest::firrtl::detail {
namespace {
using grh::LogicalType;
using grh::OperationKind;
using grh::ValueId;

struct AccessPath {
    std::string name;
    std::vector<std::string> fields;
    std::vector<ExprId> indices;
};
struct Driver {
    Leaf leaf;
    ValueId value;
    ValueId current;
    Range range;
    // Disjoint half-open bit intervals, with a sentinel at total width.
    std::map<int64_t, int> coverage;
};

class Lowerer {
public:
    Lowerer(Semantics& semantics, grh::Graph& graph, const FirrtlOptions& options)
        : semantics_(semantics), graph_(graph), options_(options) {}
    ModuleSignature run();
private:
    using LeafKey = std::pair<std::string, std::vector<std::string>>;
    Semantics& semantics_;
    grh::Graph& graph_;
    const FirrtlOptions& options_;
    std::map<LeafKey, Driver> drivers_;
    std::map<int, ValueId> guards_;
    std::string nodeKey_;
    grh::SrcLoc location(Range range) const;
    LogicalType logical(const Leaf& leaf) const;
    Leaf projected(ExprId expression, const std::vector<std::string>& fields) const;
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
    ValueId read(ValueId base, const std::vector<ExprId>& indices, Range range);
    ValueId expression(ExprId id, const std::vector<std::string>& fields = {});
    ValueId primitive(ExprId id);
    ValueId guard(int id, Range range);
    void connect(const Connection& connection);
    void cover(Driver& driver, int64_t begin, int64_t end, int guard);
    void drive(ValueId target, ValueId source, Range range, bool root = false);
    void checkCycles() const;
};

grh::SrcLoc Lowerer::location(Range range) const {
    const auto position = semantics_.source.position(range);
    grh::SrcLoc result;
    result.file = position.file.generic_string();
    result.line = static_cast<uint32_t>(position.line);
    result.column = static_cast<uint32_t>(position.column);
    result.pass = "ingest-firrtl"; result.note = position.sourceInfo;
    return result;
}
LogicalType Lowerer::logical(const Leaf& leaf) const {
    return {grh::ValueType::Logic, static_cast<int32_t>(semantics_.total(leaf)),
            semantics_.resolvedKind(leaf.type) == TypeKind::SInt, grh::LogicDomain::FourState, leaf.shape};
}
Leaf Lowerer::projected(ExprId id, const std::vector<std::string>& fields) const {
    const auto leaves = semantics_.leaves(semantics_.expressions[id]);
    const auto found = std::find_if(leaves.begin(), leaves.end(), [&](const auto& leaf) { return leaf.fields == fields; });
    if (found == leaves.end()) throw std::logic_error("invalid FIRRTL expression leaf");
    return *found;
}
ValueId Lowerer::op(OperationKind kind, const LogicalType& type, std::span<const ValueId> args, Range range) {
    if (!type.width) return {};
    const auto operation = graph_.createOperation(kind);
    const auto result = graph_.createValue(type);
    for (const auto value : args) {
        if (!value) throw std::logic_error("zero-width FIRRTL operand was not eliminated");
        graph_.addOperand(operation, value);
    }
    graph_.addResult(operation, result);
    graph_.setOpSrcLoc(operation, location(range)); graph_.setValueSrcLoc(result, location(range));
    if (!nodeKey_.empty()) node::write(graph_, operation, {{nodeKey_}, {}, {nodeKey_}});
    return result;
}
ValueId Lowerer::constant(const LogicalType& type, std::string bits, Range range) {
    const auto value = op(OperationKind::kConstant, type, {}, range);
    if (value) graph_.setAttr(graph_.valueDef(value), "constValue", std::to_string(type.width) + (type.isSigned ? "'sb" : "'b") + bits);
    return value;
}
ValueId Lowerer::number(int64_t value, int32_t width, Range range) {
    std::string bits;
    do { bits += char('0' + (value & 1)); value >>= 1; } while (value);
    std::reverse(bits.begin(), bits.end());
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
    if (expr.kind == ExprKind::Reference) { path.name = expr.name; return true; }
    if (expr.kind != ExprKind::Field && expr.kind != ExprKind::Index) return false;
    if (!access(expr.operands[0], path)) return false;
    if (expr.kind == ExprKind::Field) path.fields.emplace_back(expr.name);
    else path.indices.push_back(expr.operands[1]);
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
ValueId Lowerer::read(ValueId base, const std::vector<ExprId>& indices, Range range) {
    if (indices.empty() || !base) return base;
    const auto original = graph_.valueLogicalType(base);
    auto resultType = original;
    std::vector<ValueId> operands{base};
    ValueId valid;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto value = index(indices[i]); operands.push_back(value);
        const auto inside = inBounds(value, original.shape[i], range);
        valid = valid ? op(OperationKind::kAnd, {grh::ValueType::Logic, 1, false}, {valid, inside}, range) : inside;
        resultType.width /= original.shape[i];
    }
    resultType.shape.erase(resultType.shape.begin(), resultType.shape.begin() + indices.size());
    const auto result = op(OperationKind::kSliceArray, resultType, operands, range);
    return op(OperationKind::kMux, resultType, {valid, result, invalid(resultType, range)}, range);
}
ValueId Lowerer::expression(ExprId id, const std::vector<std::string>& fields) {
    const auto& expr = semantics_.body.arena.expressions[id];
    if (!semantics_.total(projected(id, fields))) return {};
    AccessPath path;
    if (access(id, path)) {
        path.fields.insert(path.fields.end(), fields.begin(), fields.end());
        const auto base = drivers_.at({path.name, path.fields}).value;
        if (!base) return invalid(logical(projected(id, fields)), expr.range);
        return read(base, path.indices, expr.range);
    }
    if (expr.kind == ExprKind::Field) {
        auto projection = fields; projection.insert(projection.begin(), std::string(expr.name));
        return expression(expr.operands[0], projection);
    }
    if (expr.kind == ExprKind::Index) return read(expression(expr.operands[0], fields), {expr.operands[1]}, expr.range);
    const auto type = logical(projected(id, fields));
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
    if (expr.kind == ExprKind::Primitive) return primitive(id);
    throw Error(expr.range, "FIRRTL expression lowering is not supported yet (F06)");
}

ValueId Lowerer::primitive(ExprId id) {
    const auto& expr = semantics_.body.arena.expressions[id];
    const auto word = expr.name; const auto range = expr.range;
    const auto type = logical(projected(id, {}));
    std::vector<ValueId> args;
    std::vector<int64_t> widths, parameters;
    for (const auto arg : expr.operands) {
        if (semantics_.body.arena.expressions[arg].kind == ExprKind::Integer) parameters.push_back(*semantics_.staticIndex(arg));
        else { args.push_back(expression(arg)); widths.push_back(semantics_.width(semantics_.expressions[arg])); }
    }
    if (word == "cat") {
        std::vector<ValueId> nonzero;
        for (const auto arg : args) if (arg) nonzero.push_back(arg);
        return nonzero.size() == 1 ? view(nonzero[0], type, range) : op(OperationKind::kConcat, type, nonzero, range);
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
        return op(OperationKind::kMul, type, args, range);
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

ValueId Lowerer::guard(int id, Range range) {
    if (const auto found = guards_.find(id); found != guards_.end()) return found->second;
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
    path.fields.insert(path.fields.end(), connection.sink.fields.begin(), connection.sink.fields.end());
    auto& driver = drivers_.at({path.name, path.fields});
    if (!driver.value) return;
    const auto fullType = graph_.valueLogicalType(driver.value);
    auto dataType = fullType;
    for (std::size_t i = 0; i < path.indices.size(); ++i) dataType.width /= fullType.shape[i];
    dataType.shape.erase(dataType.shape.begin(), dataType.shape.begin() + path.indices.size());
    auto data = connection.invalid ? invalid(dataType, connection.range) : adapt(expression(connection.source.expression, connection.source.fields), dataType, connection.range);
    int64_t offset = 0, stride = fullType.width; bool staticWrite = true;
    for (std::size_t i = 0; i < path.indices.size(); ++i) {
        stride /= fullType.shape[i]; const auto fixed = semantics_.staticIndex(path.indices[i]);
        if (!fixed || *fixed >= fullType.shape[i]) staticWrite = false;
        else offset += *fixed * stride;
    }
    if (staticWrite) cover(driver, offset, offset + dataType.width, connection.guard);
    auto current = driver.current;
    if (!current) current = invalid(fullType, connection.range);
    auto enable = guard(connection.guard, connection.range);
    if (!path.indices.empty()) {
        const LogicalType offsetType{grh::ValueType::Logic, 32, false};
        const LogicalType bit{grh::ValueType::Logic, 1, false};
        auto bitOffset = number(0, 32, connection.range); stride = fullType.width;
        for (std::size_t i = 0; i < path.indices.size(); ++i) {
            stride /= fullType.shape[i]; const auto value = index(path.indices[i]);
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
        const auto clear = op(OperationKind::kNot, flatType, {shifted}, connection.range);
        const auto kept = op(OperationKind::kAnd, flatType, {view(current, flatType, connection.range), clear}, connection.range);
        data = view(op(OperationKind::kOr, flatType, {kept, payload}, connection.range), fullType, connection.range);
    }
    driver.current = connection.guard == 1 && path.indices.empty() ? data :
        op(OperationKind::kMux, fullType, {enable, data, current}, connection.range);
}
void Lowerer::drive(ValueId target, ValueId source, Range range, bool root) {
    if (!target) return;
    const auto operation = graph_.createOperation(OperationKind::kAssign);
    graph_.addOperand(operation, source); graph_.addResult(operation, target); graph_.setOpSrcLoc(operation, location(range));
    if (root && !nodeKey_.empty()) node::write(graph_, operation, {{nodeKey_}, {{nodeKey_, 0}}, {nodeKey_}});
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
            if (!definition || frame.next == graph_.opOperands(definition).size()) {
                colors[frame.value] = 2; stack.pop_back(); continue;
            }
            const auto dependency = graph_.opOperands(definition)[frame.next++];
            if (colors[dependency] == 1) {
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
ModuleSignature Lowerer::run() {
    const auto& module = semantics_.body.interface;
    ModuleSignature signature; signature.name = module.name; signature.sourceUnit = semantics_.source.unit();
    signature.language = SourceLanguage::Firrtl; signature.location = semantics_.source.position(module.range);
    graph_.setMetadata("firrtl.version", std::string(semantics_.source.version()));
    graph_.setMetadata("firrtl.sourceUnit", std::string(semantics_.source.unit()));
    graph_.setMetadata("firrtl.invalidPolicy", std::string(options_.invalidPolicy == FirrtlInvalidPolicy::Unknown ? "unknown" : "zero"));
    std::set<std::string> names; std::vector<std::string> elided;
    for (const auto& name : semantics_.declarationOrder) {
        const auto& declaration = semantics_.declarations.at(name);
        for (const auto& leaf : semantics_.leaves(declaration.type, declaration.flow)) {
            auto symbol = name; for (const auto& field : leaf.fields) symbol += "__DOT__" + field;
            if (!names.insert(symbol).second) throw Error(declaration.range, "flattened FIRRTL name collision '" + symbol + "'");
            const auto type = logical(leaf); ValueId value;
            if (type.width) {
                value = graph_.createValue(graph_.internSymbol(symbol), type);
                graph_.setValueSrcLoc(value, location(declaration.range));
                if (declaration.port) {
                    if (leaf.flow == 1) graph_.bindInputPort(symbol, value);
                    else graph_.bindOutputPort(symbol, value);
                }
            } else elided.push_back(symbol);
            Driver driver{leaf, value, {}, declaration.range, {{0, 0}, {type.width, 0}}};
            drivers_.emplace(LeafKey{name, leaf.fields}, std::move(driver));
            if (declaration.port) {
                PortLeafSignature port; port.name = symbol; port.fieldPath = {name};
                port.fieldPath.insert(port.fieldPath.end(), leaf.fields.begin(), leaf.fields.end());
                port.direction = leaf.flow == 1 ? LeafDirection::Input : LeafDirection::Output;
                port.elementWidth = static_cast<int32_t>(semantics_.width(leaf.type)); port.shape = leaf.shape;
                port.isSigned = type.isSigned; port.isConst = leaf.type->constant; port.requiresTypeResolution = !type.width;
                const auto kind = semantics_.resolvedKind(leaf.type);
                port.sourceType = kind == TypeKind::SInt ? "SInt" : kind == TypeKind::Clock ? "Clock" : kind == TypeKind::AsyncReset ? "AsyncReset" : "UInt";
                signature.ports.push_back(std::move(port));
            }
        }
    }
    if (!elided.empty()) graph_.setMetadata("firrtl.elidedLeaves", std::move(elided));
    for (const auto& name : semantics_.declarationOrder) {
        const auto& declaration = semantics_.declarations.at(name);
        if (!declaration.definition) continue;
        for (const auto& leaf : semantics_.leaves(declaration.type)) {
            const auto value = drivers_.at({name, leaf.fields}).value;
            if (!value) continue;
            if (options_.nodeAnnotations) nodeKey_ = node::key({std::string(semantics_.source.unit()), std::string(semantics_.source.circuit()), std::string(module.name), {}, name, leaf.fields});
            drive(value, expression(*declaration.definition, leaf.fields), declaration.range, true);
            nodeKey_.clear();
        }
    }
    for (const auto& connection : semantics_.connections) connect(connection);
    for (auto& [key, driver] : drivers_) {
        if (!driver.value || !(driver.leaf.flow & 2)) continue;
        const auto required = semantics_.declarations.at(key.first).guard;
        for (auto at = driver.coverage.begin(); std::next(at) != driver.coverage.end(); ++at)
            if (semantics_.guards.conjunction(required, semantics_.guards.negate(at->second)))
                throw Error(driver.range, "FIRRTL component '" + key.first + "' is not fully initialized on every path");
        drive(driver.value, driver.current ? driver.current : invalid(graph_.valueLogicalType(driver.value), driver.range), driver.range);
    }
    checkCycles(); node::requireValid(graph_); return signature;
}
} // namespace

ModuleSignature lowerCombinational(const Source& source, const ModuleBody& body,
                                   grh::Design& design, const FirrtlOptions& options) {
    if (body.interface.kind != ModuleKind::Module)
        throw Error(body.interface.range, "FIRRTL external/class lowering is not supported yet (F06/F07)");
    if (!body.interface.enabledLayers.empty() || !body.interface.knownLayers.empty())
        throw Error(body.interface.range, "FIRRTL layer lowering is not supported yet (F06)");
    Semantics semantics(source, body); semantics.analyze();
    auto& graph = design.createGraph(std::string(body.interface.name));
    return Lowerer(semantics, graph, options).run();
}
} // namespace wolvrix::lib::ingest::firrtl::detail
