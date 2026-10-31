#include "semantic.hpp"

#include <algorithm>
#include <deque>
#include <limits>

namespace wolvrix::lib::ingest::firrtl::detail {
namespace {
constexpr int64_t kMaximumWidth = std::numeric_limits<int32_t>::max();
int64_t checked(int64_t width, Range range) {
    if (width < 0 || width > kMaximumWidth) throw Error(range, "FIRRTL width is outside the GRH range");
    return width;
}
bool integerKind(TypeKind kind) { return kind == TypeKind::UInt || kind == TypeKind::SInt; }
bool storable(TypePtr type) {
    if (type->constant) return false;
    if (type->kind == TypeKind::Vector) return storable(type->element);
    if (type->kind == TypeKind::Bundle)
        return std::all_of(type->fields.begin(), type->fields.end(), [](const auto& field) { return !field.flip && storable(field.type); });
    return integerKind(type->kind);
}
int reversed(int flow) { return flow == 3 ? 3 : 3 - flow; }
}

Literal literal(std::string_view text, bool isSigned, Range range) {
    Literal result;
    result.negative = !text.empty() && text.front() == '-';
    if (result.negative) text.remove_prefix(1);
    unsigned radix = 10;
    if (text.size() > 2 && text[0] == '0') {
        const auto code = text[1];
        if (code == 'h') radix = 16;
        else if (code == 'o') radix = 8;
        else if (code == 'b') radix = 2;
        else if (code == 'd') radix = 10;
        if (code == 'h' || code == 'o' || code == 'b' || code == 'd') text.remove_prefix(2);
    }
    std::vector<unsigned> bits{0};
    for (const char c : text) {
        unsigned carry = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10;
        for (auto& bit : bits) { const auto value = bit * radix + carry; bit = value & 1; carry = value >> 1; }
        while (carry) { bits.push_back(carry & 1); carry >>= 1; }
    }
    while (bits.size() > 1 && !bits.back()) bits.pop_back();
    const bool zero = bits.size() == 1 && !bits[0];
    result.negative &= !zero;
    if (result.negative && !zero && !isSigned) throw Error(range, "a UInt literal cannot be negative");
    if (isSigned && !zero) {
        const auto ones = std::count(bits.begin(), bits.end(), 1u);
        if (!result.negative || ones != 1) bits.push_back(0);
    }
    if (result.negative && !zero) {
        unsigned carry = 1;
        for (auto& bit : bits) { const unsigned value = (bit ^ 1) + carry; bit = value & 1; carry = value >> 1; }
    }
    result.minimum = static_cast<int64_t>(bits.size());
    for (auto bit = bits.rbegin(); bit != bits.rend(); ++bit) result.bits += char('0' + *bit);
    return result;
}

int Guards::make(int variable, int low, int high) {
    if (low == high) return low;
    const auto key = std::tuple{variable, low, high};
    if (const auto found = unique_.find(key); found != unique_.end()) return found->second;
    const auto id = static_cast<int>(branches.size());
    branches.push_back({variable, low, high}); unique_.emplace(key, id); return id;
}
int Guards::variable(std::string key, ExprId expression) {
    const auto [found, inserted] = variables_.emplace(std::move(key), static_cast<int>(expressions.size()));
    if (inserted) expressions.push_back(expression);
    return make(found->second, 0, 1);
}
int Guards::apply(bool conjunction, int left, int right) {
    if (left > right) std::swap(left, right);
    if (conjunction) { if (!left || right == 1 || left == right) return left; if (left == 1) return right; }
    else { if (!left || left == right) return right; if (left == 1 || right == 1) return 1; }
    const auto key = std::tuple{conjunction, left, right};
    if (const auto found = memo_.find(key); found != memo_.end()) return found->second;
    const auto a = branches[left], b = branches[right];
    const int variable = std::min(a.variable, b.variable);
    const auto low = apply(conjunction, a.variable == variable ? a.low : left, b.variable == variable ? b.low : right);
    const auto high = apply(conjunction, a.variable == variable ? a.high : left, b.variable == variable ? b.high : right);
    const auto result = make(variable, low, high); memo_.emplace(key, result); return result;
}
int Guards::conjunction(int left, int right) { return apply(true, left, right); }
int Guards::disjunction(int left, int right) { return apply(false, left, right); }
int Guards::negate(int value) {
    if (value < 2) return 1 - value;
    const auto branch = branches[value]; return make(branch.variable, negate(branch.low), negate(branch.high));
}

Semantics::Semantics(const Source& source, const ModuleBody& body, const FirrtlOptions& options,
                     const std::map<std::string, ModuleSignature>* interfaces)
    : source(source), body(body), expressions(body.arena.expressions.size()), enabledLayers_(options.enabledLayers), interfaces_(interfaces) {
    const auto visit = [&](auto&& self, std::span<const Layer> layers, const std::string& parent) -> void {
        for (const auto& layer : layers) {
            const auto path = parent.empty() ? std::string(layer.name) : parent + "." + std::string(layer.name);
            knownLayers_.insert(path); self(self, layer.children, path);
        }
    };
    visit(visit, source.layers(), {});
    for (const auto& path : enabledLayers_) if (!knownLayers_.contains(path))
        throw Error(body.interface.range, "unknown enabled FIRRTL layer '" + path + "'");
    for (const auto path : body.interface.enabledLayers) {
        if (!knownLayers_.contains(std::string(path))) throw Error(body.interface.range, "unknown module enabled layer");
        if (!layerEnabled(path)) throw Error(body.interface.range, "module requires explicitly enabled layer '" + std::string(path) + "'");
    }
}
bool Semantics::layerEnabled(std::string_view path) const {
    if (path.empty()) return true;
    return std::any_of(enabledLayers_.begin(), enabledLayers_.end(), [&](const auto& enabled) { return enabled == path || enabled.starts_with(std::string(path) + "."); });
}
std::string Semantics::layerPath(const Statement& statement) const {
    const auto path = activeLayer_.empty() ? std::string(statement.target) : activeLayer_ + "." + std::string(statement.target);
    if (!knownLayers_.contains(path)) throw Error(statement.range, "unknown or incorrectly nested layerblock '" + path + "'");
    return path;
}
uint32_t Semantics::newWidth(int64_t value, Range range) {
    const auto id = static_cast<uint32_t>(widths_.size());
    widths_.push_back({value, value >= 0, range, {}}); return id;
}
uint32_t Semantics::newReset(unsigned flags) {
    const auto id = static_cast<uint32_t>(resetParents_.size()); resetParents_.push_back(id); resetFlags_.push_back(flags); return id;
}
uint32_t Semantics::resetRoot(uint32_t id) {
    if (resetParents_[id] != id) resetParents_[id] = resetRoot(resetParents_[id]);
    return resetParents_[id];
}
unsigned Semantics::resetFlags(uint32_t id) const {
    while (resetParents_[id] != id) id = resetParents_[id]; return resetFlags_[id];
}
void Semantics::unifyReset(TypePtr left, TypePtr right, Range range) {
    const auto a = resetRoot(left->reset), b = resetRoot(right->reset);
    resetParents_[b] = a; resetFlags_[a] |= resetFlags_[b];
    if (resetFlags_[a] == 3) throw Error(range, "Reset is connected to both synchronous and asynchronous resets");
    for (const auto& type : {left, right}) if (type->kind == TypeKind::UInt) {
        constrain(type->width, {}, [](const auto&) { return 1; }, range);
        checks_.push_back([this, type, range] { if (width(type) != 1) throw Error(range, "Reset requires a one-bit UInt"); });
    }
}
void Semantics::constrain(uint32_t result, std::vector<uint32_t> args,
                          std::function<int64_t(const std::vector<int64_t>&)> calculate, Range range) {
    if (widths_[result].fixed) return;
    const auto id = constraints_.size();
    for (const auto arg : args) widths_[arg].users.push_back(id);
    constraints_.push_back({result, std::move(args), std::move(calculate), range});
}
TypePtr Semantics::ground(TypeKind kind, int64_t bits, Range range, bool constant) {
    if (bits >= 0) checked(bits, range);
    auto result = std::make_shared<SemanticType>(); result->kind = kind; result->constant = constant;
    result->width = newWidth(bits, range);
    result->reset = newReset(kind == TypeKind::AsyncReset ? 2 : kind == TypeKind::UInt ? 1 : 0);
    return result;
}
TypePtr Semantics::declaredType(const Arena& arena, TypeId id, Range range, bool constant) {
    const auto& type = arena.types.at(id); constant |= type.constant;
    if (type.kind == TypeKind::Alias) {
        const auto found = std::find_if(source.aliases().begin(), source.aliases().end(), [&](const auto& alias) { return alias.name == type.name; });
        if (found == source.aliases().end()) throw Error(range, "unknown type alias");
        return declaredType(source.arena(), found->type, range, constant);
    }
    if (integerKind(type.kind)) return ground(type.kind, type.width, range, constant);
    if (type.kind == TypeKind::Clock || type.kind == TypeKind::Reset || type.kind == TypeKind::AsyncReset)
        return ground(type.kind, 1, range, constant);
    auto result = std::make_shared<SemanticType>(); result->kind = type.kind; result->constant = constant;
    if (type.kind == TypeKind::Probe || type.kind == TypeKind::RWProbe) {
        result->element = declaredType(arena, type.element, range, constant); result->layer = type.layer;
        if (!result->layer.empty() && !knownLayers_.contains(result->layer)) throw Error(range, "unknown probe layer color");
    } else if (type.kind == TypeKind::Vector) { result->extent = type.extent; result->element = declaredType(arena, type.element, range, constant); }
    else if (type.kind == TypeKind::Bundle) {
        for (const auto& field : type.fields) result->fields.push_back({std::string(field.name), field.flip, declaredType(arena, field.type, range, constant)});
    } else throw Error(range, "FIRRTL type lowering is not supported yet for this construct (F06/F07)");
    return result;
}
bool Semantics::passive(TypePtr type) const {
    if (type->element && !passive(type->element)) return false;
    for (const auto& field : type->fields) if (field.flip || !passive(field.type)) return false;
    return true;
}
void Semantics::equivalent(TypePtr left, TypePtr right, Range range) {
    const auto resetCompatible = [](TypeKind kind) { return kind == TypeKind::UInt || kind == TypeKind::Reset || kind == TypeKind::AsyncReset; };
    if ((left->kind == TypeKind::Reset || right->kind == TypeKind::Reset) && resetCompatible(left->kind) && resetCompatible(right->kind)) {
        unifyReset(left, right, range); return;
    }
    if (left->kind != right->kind) throw Error(range, "FIRRTL operands have non-equivalent types or signedness");
    if (left->kind == TypeKind::Vector) {
        if (left->extent != right->extent) throw Error(range, "FIRRTL Vector lengths do not match");
        equivalent(left->element, right->element, range);
    } else if (left->kind == TypeKind::Bundle) {
        if (left->fields.size() != right->fields.size()) throw Error(range, "FIRRTL Bundle fields do not match");
        for (std::size_t i = 0; i < left->fields.size(); ++i) {
            const auto& a = left->fields[i]; const auto& b = right->fields[i];
            if (a.name != b.name || a.flip != b.flip) throw Error(range, "FIRRTL Bundle field order or flips do not match");
            equivalent(a.type, b.type, range);
        }
    }
}
TypePtr Semantics::merge(TypePtr left, TypePtr right, Range range) {
    equivalent(left, right, range);
    auto result = std::make_shared<SemanticType>(*left); result->constant = left->constant && right->constant;
    if (left->kind == TypeKind::Vector) result->element = merge(left->element, right->element, range);
    else if (left->kind == TypeKind::Bundle) {
        for (std::size_t i = 0; i < left->fields.size(); ++i) result->fields[i].type = merge(left->fields[i].type, right->fields[i].type, range);
    } else {
        result->width = newWidth(-1, range);
        constrain(result->width, {left->width, right->width}, [](const auto& args) { return std::max(args[0], args[1]); }, range);
    }
    return result;
}

void Semantics::collect(std::span<const StmtId> statements) {
    for (const auto id : statements) {
        const auto& statement = body.arena.statements[id];
        activeStatements.insert(id);
        if (statement.kind == StmtKind::LayerBlock) {
            const auto path = layerPath(statement);
            if (layerEnabled(path)) { auto previous = activeLayer_; activeLayer_ = path; collect(statement.body); activeLayer_ = std::move(previous); }
            continue;
        }
        if (statement.kind == StmtKind::Wire || statement.kind == StmtKind::Reg || statement.kind == StmtKind::RegReset) {
            Declaration declaration{std::string(statement.name), declaredType(body.arena, statement.type, statement.range), statement.range};
            declaration.layer = activeLayer_;
            if (statement.kind != StmtKind::Wire) {
                if (!storable(declaration.type)) throw Error(statement.range, "register data must have a passive, non-const storable type");
                declaration.state = Declaration::Register{statement.operands[0]};
                if (statement.kind == StmtKind::RegReset) {
                    declaration.state->reset = statement.operands[1];
                    declaration.state->initial = statement.operands[2];
                }
            }
            declarations.emplace(declaration.name, declaration); declarationOrder.push_back(declaration.name);
        } else if (statement.kind == StmtKind::Memory || statement.kind == StmtKind::CMem || statement.kind == StmtKind::SMem) {
            collectMemory(statement);
        } else if (statement.kind == StmtKind::MPort) {
            collectMemoryPort(statement);
        } else if (statement.kind == StmtKind::Node) {
            nodeDeclarations_.emplace(statement.name, id); declarationOrder.emplace_back(statement.name);
            nodeLayers_.emplace(statement.name, activeLayer_);
        } else if (statement.kind == StmtKind::Instance) {
            const auto target = source.findModule(statement.target);
            const auto& signature = source.modules()[*target];
            auto type = std::make_shared<SemanticType>(); type->kind = TypeKind::Bundle;
            for (const auto& port : signature.ports)
                type->fields.push_back({std::string(port.name), port.direction == LeafDirection::Input,
                    declaredType(source.arena(), port.type, port.range)});
            if (interfaces_ && interfaces_->contains(std::string(statement.target))) {
                const auto& resolved = interfaces_->at(std::string(statement.target));
                for (const auto& leaf : leaves(type)) {
                    const auto port = std::find_if(resolved.ports.begin(), resolved.ports.end(),
                        [&](const auto& entry) { return entry.fieldPath == leaf.fields; });
                    if (port == resolved.ports.end()) throw Error(statement.range, "instance port missing from resolved interface");
                    if (port->shape != leaf.shape) throw Error(statement.range, "instance array geometry differs from resolved interface");
                    constrain(leaf.type->width, {}, [bits = port->elementWidth](const auto&) { return bits; }, statement.range);
                    if (leaf.type->kind == TypeKind::Reset)
                        unifyReset(leaf.type, ground(port->sourceType == "AsyncReset" ? TypeKind::AsyncReset : TypeKind::UInt, 1, statement.range), statement.range);
                }
            }
            Declaration declaration{std::string(statement.name), type, statement.range, 1};
            declaration.layer = activeLayer_;
            declarations.emplace(declaration.name, declaration); declarationOrder.push_back(declaration.name);
            instances.push_back({declaration.name, std::string(statement.target), statement.range});
        } else if (statement.kind != StmtKind::Connect && statement.kind != StmtKind::Define && statement.kind != StmtKind::Invalidate && statement.kind != StmtKind::When && statement.kind != StmtKind::Skip &&
                   statement.kind != StmtKind::Printf && statement.kind != StmtKind::Stop && statement.kind != StmtKind::Assert && statement.kind != StmtKind::Assume &&
                   statement.kind != StmtKind::Cover && statement.kind != StmtKind::Intrinsic) {
            throw Error(statement.range, "FIRRTL statement lowering is not supported yet for this construct (F06)");
        }
        collect(statement.body); collect(statement.otherwise);
    }
}

TypePtr Semantics::expression(ExprId id) {
    if (expressions.at(id)) return expressions[id];
    const auto& expr = body.arena.expressions[id];
    TypePtr result;
    if (expr.kind == ExprKind::Reference) {
        const std::string name(expr.name);
        if (!declarations.contains(name)) {
            const auto node = nodeDeclarations_.find(name);
            if (node == nodeDeclarations_.end()) throw Error(expr.range, "unknown FIRRTL component");
            if (!activeNodes_.insert(name).second) throw Error(expr.range, "cyclic node definition");
            const auto& definition = body.arena.statements[node->second];
            auto previous = activeLayer_; activeLayer_ = nodeLayers_.at(name);
            auto type = expression(definition.operands[0]);
            activeLayer_ = std::move(previous);
            if (!passive(type)) throw Error(definition.range, "a node expression must have a passive type");
            declarations.emplace(name, Declaration{name, type, definition.range, 1, definition.operands[0]});
            declarations.at(name).layer = nodeLayers_.at(name);
            activeNodes_.erase(name);
        }
        result = declarations.at(name).type;
    } else if (expr.kind == ExprKind::Field) {
        const auto base = expression(expr.operands[0]);
        if (base->kind != TypeKind::Bundle) throw Error(expr.range, "field selection requires a Bundle");
        const auto field = std::find_if(base->fields.begin(), base->fields.end(), [&](const auto& field) { return field.name == expr.name; });
        if (field == base->fields.end()) throw Error(expr.range, "unknown Bundle field '" + std::string(expr.name) + "'");
        result = field->type;
    } else if (expr.kind == ExprKind::Index) {
        const auto base = expression(expr.operands[0]);
        if (base->kind != TypeKind::Vector) throw Error(expr.range, "index selection requires a Vector");
        const auto& index = body.arena.expressions[expr.operands[1]];
        if (index.kind == ExprKind::Integer) {
            const auto value = integer64({TokenKind::Number, index.literal, index.range}, true);
            if (value >= base->extent) throw Error(index.range, "static Vector index is out of range");
        } else {
            auto type = expression(expr.operands[1]);
            if (type->kind != TypeKind::UInt) throw Error(index.range, "dynamic Vector index must have UInt type");
        }
        result = base->element;
    } else if (expr.kind == ExprKind::UInt || expr.kind == ExprKind::SInt) {
        result = declaredType(body.arena, expr.type, expr.range, true);
        const auto value = literal(expr.literal, result->kind == TypeKind::SInt, expr.range);
        auto& bits = widths_[result->width];
        if (!bits.fixed) { bits.value = value.minimum; bits.fixed = true; }
        else if (bits.value < value.minimum && !(bits.value == 0 && value.bits == "0"))
            throw Error(expr.range, "integer literal does not fit its explicit FIRRTL width");
    } else if (expr.kind == ExprKind::Mux) {
        const auto select = expression(expr.operands[0]);
        if (select->kind != TypeKind::UInt) throw Error(expr.range, "mux selector must have UInt type");
        checks_.push_back([this, select, range = expr.range] { if (width(select) > 1) throw Error(range, "mux selector must have at most one bit"); });
        const auto a = expression(expr.operands[1]), b = expression(expr.operands[2]);
        if (!passive(a) || !passive(b)) throw Error(expr.range, "mux arguments must have passive types");
        result = merge(a, b, expr.range);
        if (!select->constant) {
            const auto clear = [](auto&& self, TypePtr type) -> void {
                type->constant = false;
                if (type->element) self(self, type->element);
                for (const auto& field : type->fields) self(self, field.type);
            };
            clear(clear, result);
        }
    } else if (expr.kind == ExprKind::Probe || expr.kind == ExprKind::RWProbe) {
        auto element = expression(expr.operands[0]);
        const auto unflip = [&](auto&& self, TypePtr type) -> TypePtr {
            auto copy = std::make_shared<SemanticType>(*type);
            if (type->element) copy->element = self(self, type->element);
            for (auto& field : copy->fields) { field.flip = false; field.type = self(self, field.type); }
            return copy;
        };
        result = std::make_shared<SemanticType>();
        result->kind = expr.kind == ExprKind::Probe ? TypeKind::Probe : TypeKind::RWProbe; result->element = unflip(unflip, element);
    } else if (expr.kind == ExprKind::Read) {
        auto probe = expression(expr.operands[0]);
        if (probe->kind != TypeKind::Probe && probe->kind != TypeKind::RWProbe) throw Error(expr.range, "read requires a probe");
        if (!layerEnabled(probe->layer)) throw Error(expr.range, "probe color is not enabled");
        if (!probe->layer.empty() && activeLayer_ != probe->layer && !activeLayer_.starts_with(probe->layer + ".") &&
            std::find(body.interface.enabledLayers.begin(), body.interface.enabledLayers.end(), probe->layer) == body.interface.enabledLayers.end())
            throw Error(expr.range, "probe read is outside its layer color");
        result = probe->element;
    } else if (expr.kind == ExprKind::Primitive) {
        const auto word = expr.name;
        std::vector<TypePtr> args;
        std::vector<uint32_t> widths;
        std::vector<int64_t> parameters;
        bool constant = true;
        for (const auto arg : expr.operands) {
            const auto& operand = body.arena.expressions[arg];
            if (operand.kind == ExprKind::Integer) parameters.push_back(integer64({TokenKind::Number, operand.literal, operand.range}, true));
            else { args.push_back(expression(arg)); widths.push_back(args.back()->width); constant &= args.back()->constant; }
        }
        const bool cast = word == "asUInt" || word == "asSInt" || word == "asClock" || word == "asAsyncReset" || word == "asReset";
        for (const auto& arg : args) {
            if ((!cast && !integerKind(arg->kind)) || (cast && arg->kind != TypeKind::UInt && arg->kind != TypeKind::SInt && arg->kind != TypeKind::Clock && arg->kind != TypeKind::Reset && arg->kind != TypeKind::AsyncReset))
                throw Error(expr.range, "primitive requires legal ground operands");
        }
        const bool shift = word == "dshl" || word == "dshr";
        if (args.size() > 1) {
            if (shift) { if (args[1]->kind != TypeKind::UInt) throw Error(expr.range, "dynamic shift amount must have UInt type"); }
            else for (const auto& arg : args)
                if (args[0]->kind != arg->kind) throw Error(expr.range, "primitive operands must have the same signedness");
        }
        TypeKind kind = args.empty() ? TypeKind::UInt : args[0]->kind;
        if (word == "eq" || word == "neq" || word == "lt" || word == "leq" || word == "gt" || word == "geq" || word == "and" || word == "or" || word == "xor" || word == "not" || word == "cat" || word == "bits" || word == "head" || word == "tail" || word == "andr" || word == "orr" || word == "xorr" || word == "asUInt") kind = TypeKind::UInt;
        if (word == "cvt" || word == "neg" || word == "asSInt") kind = TypeKind::SInt;
        if (word == "asClock") kind = TypeKind::Clock;
        if (word == "asAsyncReset") kind = TypeKind::AsyncReset;
        if (word == "asReset") kind = TypeKind::Reset;
        if (word == "asReset" && args[0]->kind != TypeKind::UInt) throw Error(expr.range, "asReset requires UInt<1>");
        if (word == "asReset") checks_.push_back([this, arg = args[0], range = expr.range] { if (width(arg) != 1) throw Error(range, "asReset requires UInt<1>"); });
        const bool signedArgument = !args.empty() && args[0]->kind == TypeKind::SInt;
        auto calculate = [word, parameters, signedArgument, range = expr.range](const std::vector<int64_t>& w) -> int64_t {
            const int64_t a = w.empty() ? 0 : w[0], b = w.size() > 1 ? w[1] : 0;
            if (word == "add" || word == "sub") return checked(std::max(a, b) + 1, range);
            if (word == "mul") return checked(a + b, range);
            if (word == "div") return checked(a + signedArgument, range);
            if (word == "rem") return std::min(a, b);
            if (word == "eq" || word == "neq" || word == "lt" || word == "leq" || word == "gt" || word == "geq" || word == "andr" || word == "orr" || word == "xorr" || word == "asClock" || word == "asAsyncReset" || word == "asReset") return 1;
            if (word == "and" || word == "or" || word == "xor") return std::max(a, b);
            if (word == "cat") { int64_t sum = 0; for (const auto width : w) sum = checked(sum + width, range); return sum; }
            if (word == "cvt") return checked(a + !signedArgument, range);
            if (word == "neg") return checked(a + 1, range);
            if (word == "pad") return checked(std::max(a, parameters[0]), range);
            if (word == "shl") {
                if (parameters[0] > kMaximumWidth - a) throw Error(range, "FIRRTL shift width is outside the GRH range");
                return a + parameters[0];
            }
            if (word == "shr") return std::max<int64_t>(signedArgument ? 1 : 0, a - parameters[0]);
            if (word == "dshl") {
                if (b > 31) throw Error(range, "dynamic shift result width exceeds GRH range");
                return checked(a + (int64_t{1} << b) - 1, range);
            }
            if (word == "bits") {
                if (parameters[0] < parameters[1] || parameters[0] >= a) throw Error(range, "bits range is outside its operand width");
                return checked(parameters[0] - parameters[1] + 1, range);
            }
            if (word == "head" || word == "tail") {
                if (parameters[0] > a) throw Error(range, "head/tail parameter exceeds operand width");
                return word == "head" ? parameters[0] : a - parameters[0];
            }
            return a;
        };
        result = ground(kind, -1, expr.range, constant); constrain(result->width, widths, std::move(calculate), expr.range);
    } else throw Error(expr.range, "FIRRTL expression lowering is not supported yet for this construct (F06)");
    expressions[id] = result; return result;
}

int Semantics::flow(ExprId id) const {
    const auto& expr = body.arena.expressions[id];
    if (expr.kind == ExprKind::Reference) return declarations.at(std::string(expr.name)).flow;
    if (expr.kind == ExprKind::Index) return flow(expr.operands[0]);
    if (expr.kind == ExprKind::Field) {
        const auto base = expressions[expr.operands[0]];
        const auto field = std::find_if(base->fields.begin(), base->fields.end(), [&](const auto& field) { return field.name == expr.name; });
        return field->flip ? reversed(flow(expr.operands[0])) : flow(expr.operands[0]);
    }
    return 1;
}
void Semantics::connect(Projection left, TypePtr leftType, Projection right, TypePtr rightType,
                        int guard, Range range, bool invalid) {
    if (leftType->kind == TypeKind::Probe || leftType->kind == TypeKind::RWProbe) {
        if (invalid) return;
        throw Error(range, "probes require define, not connect");
    }
    if (!invalid) equivalent(leftType, rightType, range);
    if (leftType->kind == TypeKind::Vector) {
        connect(std::move(left), leftType->element, std::move(right), invalid ? nullptr : rightType->element, guard, range, invalid); return;
    }
    if (leftType->kind == TypeKind::Bundle) {
        for (std::size_t i = 0; i < leftType->fields.size(); ++i) {
            const auto& a = leftType->fields[i]; auto l = left, r = right;
            l.fields.push_back(a.name); r.fields.push_back(a.name);
            if (a.flip && !invalid) connect(std::move(r), rightType->fields[i].type, std::move(l), a.type, guard, range);
            else connect(std::move(l), a.type, std::move(r), invalid ? nullptr : rightType->fields[i].type, guard, range, invalid);
        }
        return;
    }
    int destination = flow(left.expression);
    auto type = expressions[left.expression];
    for (const auto& fieldName : left.fields) {
        while (type->kind == TypeKind::Vector) type = type->element;
        const auto field = std::find_if(type->fields.begin(), type->fields.end(), [&](const auto& field) { return field.name == fieldName; });
        if (field->flip) destination = reversed(destination); type = field->type;
    }
    if (!(destination & 2)) {
        if (invalid) return; // Invalidating an aggregate only acts on sink leaves.
        throw Error(range, "connect destination does not have sink or duplex flow");
    }
    auto root = left.expression;
    while (body.arena.expressions[root].kind == ExprKind::Index || body.arena.expressions[root].kind == ExprKind::Field)
        root = body.arena.expressions[root].operands[0];
    const auto& base = body.arena.expressions[root];
    if (base.kind != ExprKind::Reference) throw Error(range, "connect requires a component destination");
    if (!activeLayer_.empty() && declarations.at(std::string(base.name)).layer != activeLayer_)
        throw Error(range, "layerblock cannot drive a component declared outside its layer");
    const auto port = memoryPorts.find(std::string(base.name));
    const auto depth = port == memoryPorts.end() ? declarations.at(std::string(base.name)).conditionDepth : 0;
    // Hardware declarations are unconditional; only interleaving conditions
    // (blocks not containing the destination declaration) control a connect.
    guard = 1;
    for (std::size_t i = depth; i < conditionGuards_.size(); ++i) guard = guards.conjunction(guard, conditionGuards_[i]);
    if (!invalid) {
        if (leftType->constant && !rightType->constant) throw Error(range, "non-constant source drives a const destination");
        if (leftType->constant)
            for (const auto& condition : conditions_)
                if (!condition->constant) throw Error(range, "non-constant when predicate controls a const destination");
        constrain(leftType->width, {rightType->width}, [](const auto& args) { return args[0]; }, range);
    }
    connections.push_back({std::move(left), std::move(right), guard, invalid, range});
}
std::string Semantics::expressionKey(ExprId id) const {
    const auto& expr = body.arena.expressions[id];
    std::string result = std::to_string(static_cast<int>(expr.kind)) + ":" + std::to_string(expr.type) + ":";
    const auto append = [&](std::string_view part) { result += std::to_string(part.size()) + ":"; result += part; };
    append(expr.name); append(expr.literal);
    for (const auto arg : expr.operands) append(expressionKey(arg));
    return result;
}
void Semantics::statements(std::span<const StmtId> statements, int guard) {
    for (const auto id : statements) {
        const auto& statement = body.arena.statements[id];
        if (!activeStatements.contains(id)) continue;
        if (statement.kind == StmtKind::LayerBlock) {
            const auto path = layerPath(statement);
            if (layerEnabled(path)) { auto previous = activeLayer_; activeLayer_ = path; this->statements(statement.body, guard); activeLayer_ = std::move(previous); }
            continue;
        }
        if (statement.kind == StmtKind::Wire || statement.kind == StmtKind::Reg || statement.kind == StmtKind::RegReset) {
            auto& declaration = declarations.at(std::string(statement.name));
            declaration.conditionDepth = conditions_.size();
            if (declaration.state) {
                const auto clock = expression(declaration.state->clock);
                if (clock->kind != TypeKind::Clock) throw Error(statement.range, "register clock must have Clock type");
                if (declaration.state->reset) {
                    const auto reset = expression(*declaration.state->reset);
                    if (reset->kind != TypeKind::UInt && reset->kind != TypeKind::Reset && reset->kind != TypeKind::AsyncReset)
                        throw Error(statement.range, "register reset must have Reset, AsyncReset or one-bit UInt type");
                    constrain(reset->width, {}, [](const auto&) { return 1; }, statement.range);
                    const auto initial = expression(*declaration.state->initial);
                    equivalent(declaration.type, initial, statement.range);
                    const auto data = leaves(declaration.type), values = leaves(initial);
                    for (std::size_t i = 0; i < data.size(); ++i)
                        constrain(data[i].type->width, {values[i].type->width}, [](const auto& args) { return args[0]; }, statement.range);
                    checks_.push_back([this, reset, initial, range = statement.range] {
                        if (width(reset) != 1) throw Error(range, "register reset requires a one-bit UInt");
                        if (resolvedKind(reset) == TypeKind::AsyncReset)
                            for (const auto& leaf : leaves(initial))
                                if (!leaf.type->constant) throw Error(range, "asynchronous reset value must have const type");
                    });
                }
            }
        } else if (statement.kind == StmtKind::Node) {
            auto type = expression(statement.operands[0]);
            if (!passive(type)) throw Error(statement.range, "a node expression must have a passive type");
            declarations.try_emplace(std::string(statement.name), Declaration{std::string(statement.name), type, statement.range, 1, statement.operands[0]});
            declarations.at(std::string(statement.name)).layer = activeLayer_;
        } else if (statement.kind == StmtKind::MPort) {
            const auto& entry = memoryPorts.at(std::string(statement.name));
            auto& port = memories.at(entry.first).ports[entry.second]; port.guard = guard;
            const auto& addressExpr = body.arena.expressions[*port.address];
            auto address = addressExpr.kind == ExprKind::Integer ? ground(TypeKind::UInt, 64, addressExpr.range, true) : expression(*port.address);
            expressions[*port.address] = address;
            auto clock = expression(*port.clock);
            if (clock->kind != TypeKind::Clock) throw Error(statement.range, "mport clock must have Clock type");
            if (address->kind != TypeKind::UInt && body.arena.expressions[*port.address].kind != ExprKind::Integer)
                throw Error(statement.range, "mport address must have UInt type");
        } else if (statement.kind == StmtKind::Define) {
            auto a = expression(statement.operands[0]), b = expression(statement.operands[1]);
            if ((a->kind != TypeKind::Probe && a->kind != TypeKind::RWProbe) || (b->kind != TypeKind::Probe && b->kind != TypeKind::RWProbe))
                throw Error(statement.range, "define requires probe operands");
            if (a->kind == TypeKind::RWProbe && b->kind != TypeKind::RWProbe) throw Error(statement.range, "read-only probe cannot define RWProbe");
            if (!layerEnabled(a->layer)) throw Error(statement.range, "probe definition color is not enabled");
            if (!(flow(statement.operands[0]) & 2)) throw Error(statement.range, "probe definition requires a sink or duplex destination");
            auto root = statement.operands[0];
            while (body.arena.expressions[root].kind == ExprKind::Index || body.arena.expressions[root].kind == ExprKind::Field)
                root = body.arena.expressions[root].operands[0];
            const auto& destination = body.arena.expressions[root];
            const bool local = destination.kind == ExprKind::Reference && declarations.at(std::string(destination.name)).layer == activeLayer_;
            if (!activeLayer_.empty() && !local && a->layer != activeLayer_)
                throw Error(statement.range, "probe definition in a layerblock requires the same layer color");
            if (!b->layer.empty() && a->layer != b->layer && !a->layer.starts_with(b->layer + "."))
                throw Error(statement.range, "probe definition would expose a disabled source color");
            equivalent(a->element, b->element, statement.range);
            const auto left = leaves(a->element), right = leaves(b->element);
            for (std::size_t i = 0; i < left.size(); ++i) {
                constrain(left[i].type->width, {right[i].type->width}, [](const auto& args) { return args[0]; }, statement.range);
                checks_.push_back([this, a = left[i].type, b = right[i].type, range = statement.range] {
                    if (width(a) != width(b)) throw Error(range, "probe definition requires matching element widths");
                });
                connections.push_back({{statement.operands[0], left[i].fields}, {statement.operands[1], right[i].fields}, 1, false, statement.range, true});
            }
        } else if (statement.kind == StmtKind::Connect) {
            auto a = expression(statement.operands[0]), b = expression(statement.operands[1]);
            if (a->kind == TypeKind::Probe || a->kind == TypeKind::RWProbe || b->kind == TypeKind::Probe || b->kind == TypeKind::RWProbe)
                throw Error(statement.range, "probes require define, not connect");
            connect({statement.operands[0], {}}, a, {statement.operands[1], {}}, b, guard, statement.range);
        } else if (statement.kind == StmtKind::Invalidate) {
            auto type = expression(statement.operands[0]);
            connect({statement.operands[0], {}}, type, {}, nullptr, guard, statement.range, true);
        } else if (statement.kind == StmtKind::When) {
            auto type = expression(statement.operands[0]);
            if (type->kind != TypeKind::UInt) throw Error(statement.range, "when predicate must have UInt type");
            checks_.push_back([this, type, range = statement.range] { if (width(type) > 1) throw Error(range, "when predicate must have at most one bit"); });
            const auto fixed = staticIndex(statement.operands[0]);
            const int condition = fixed ? (*fixed != 0) : guards.variable(expressionKey(statement.operands[0]), statement.operands[0]);
            conditions_.push_back(type);
            conditionGuards_.push_back(condition);
            this->statements(statement.body, guards.conjunction(guard, condition));
            conditionGuards_.back() = guards.negate(condition);
            this->statements(statement.otherwise, guards.conjunction(guard, guards.negate(condition)));
            conditionGuards_.pop_back();
            conditions_.pop_back();
        } else if (statement.kind == StmtKind::Printf || statement.kind == StmtKind::Stop || statement.kind == StmtKind::Assert ||
                   statement.kind == StmtKind::Assume || statement.kind == StmtKind::Cover || statement.kind == StmtKind::Intrinsic) {
            auto operands = statement.operands;
            if (statement.kind == StmtKind::Intrinsic) {
                const auto& call = body.arena.expressions[operands.at(0)];
                if (call.name != "circt_chisel_ifelsefatal" || call.type || call.operands.size() != 3)
                    throw Error(statement.range, "unsupported effect intrinsic '" + std::string(call.name) + "'");
                bool format = false;
                for (const auto& parameter : call.parameters) {
                    if ((parameter.name != "format" && parameter.name != "label") || parameter.kind != LiteralKind::String)
                        throw Error(parameter.range, "unsupported ifelsefatal parameter");
                    format |= parameter.name == "format";
                }
                if (!format) throw Error(statement.range, "ifelsefatal requires a format parameter");
                operands = call.operands;
            }
            if (expression(operands[0])->kind != TypeKind::Clock) throw Error(statement.range, "effect clock must have Clock type");
            const auto booleans = statement.kind == StmtKind::Assert || statement.kind == StmtKind::Assume || statement.kind == StmtKind::Cover || statement.kind == StmtKind::Intrinsic ? 3u : 2u;
            for (std::size_t i = 1; i < booleans; ++i) {
                auto type = expression(operands[i]);
                if (type->kind != TypeKind::UInt) throw Error(statement.range, "effect predicate and enable must have UInt<1> type");
                constrain(type->width, {}, [](const auto&) { return 1; }, statement.range);
                checks_.push_back([this, type, range = statement.range] { if (width(type) != 1) throw Error(range, "effect predicate and enable must have UInt<1> type"); });
            }
            if (statement.kind == StmtKind::Printf || statement.kind == StmtKind::Assert || statement.kind == StmtKind::Assume) {
                const auto start = statement.kind == StmtKind::Printf ? 3u : 4u;
                for (std::size_t i = start; i < operands.size(); ++i) {
                    auto type = expression(operands[i]);
                    if (!integerKind(type->kind) && type->kind != TypeKind::Clock && type->kind != TypeKind::Reset && type->kind != TypeKind::AsyncReset)
                        throw Error(statement.range, "print argument must have a ground hardware type");
                }
            }
            effects.push_back({id, guard});
        }
    }
}
void Semantics::solve() {
    std::deque<std::size_t> pending;
    std::vector<bool> queued(constraints_.size(), true);
    std::size_t steps = 0;
    const auto limit = std::max<std::size_t>(1000000, constraints_.size() * 64);
    for (std::size_t i = 0; i < constraints_.size(); ++i) pending.push_back(i);
    while (!pending.empty()) {
        const auto id = pending.front(); pending.pop_front(); queued[id] = false;
        const auto& constraint = constraints_[id];
        if (++steps > limit)
            throw Error(constraint.range, "FIRRTL width inference resource limit exceeded (cyclic constraints may be unbounded)");
        std::vector<int64_t> args; bool ready = true;
        for (const auto arg : constraint.arguments) { args.push_back(widths_[arg].value); ready &= args.back() >= 0; }
        if (!ready) continue;
        auto& result = widths_[constraint.result];
        const auto next = checked(constraint.calculate(args), constraint.range);
        if (result.value >= next) continue;
        result.value = next;
        for (const auto user : result.users) if (!queued[user]) { queued[user] = true; pending.push_back(user); }
    }
    for (const auto& bits : widths_) if (bits.value < 0) throw Error(bits.range, "FIRRTL width cannot be inferred from its drivers");
    for (const auto& check : checks_) check();
}
void Semantics::normalizeGuards() {
    Guards normalized;
    std::unordered_map<ExprId, int> predicates;
    std::function<int(ExprId)> predicate = [&](ExprId id) {
        if (const auto found = predicates.find(id); found != predicates.end()) return found->second;
        const auto& expr = body.arena.expressions[id];
        int result;
        if (!width(expressions[id])) result = 0;
        else if (expr.kind == ExprKind::UInt || expr.kind == ExprKind::SInt)
            result = literal(expr.literal, expr.kind == ExprKind::SInt, expr.range).bits.back() == '1';
        else if (expr.kind == ExprKind::Primitive && expr.name == "not") result = normalized.negate(predicate(expr.operands[0]));
        else if (expr.kind == ExprKind::Primitive && (expr.name == "and" || expr.name == "or" || expr.name == "xor")) {
            const auto a = predicate(expr.operands[0]), b = predicate(expr.operands[1]);
            if (expr.name == "and") result = normalized.conjunction(a, b);
            else if (expr.name == "or") result = normalized.disjunction(a, b);
            else result = normalized.disjunction(normalized.conjunction(a, normalized.negate(b)), normalized.conjunction(normalized.negate(a), b));
        } else result = normalized.variable(expressionKey(id), id);
        predicates.emplace(id, result); return result;
    };
    std::map<int, int> converted{{0, 0}, {1, 1}};
    std::function<int(int)> convert = [&](int id) {
        if (const auto found = converted.find(id); found != converted.end()) return found->second;
        const auto branch = guards.branches[id];
        const auto condition = predicate(guards.expressions[branch.variable]);
        const auto result = normalized.disjunction(normalized.conjunction(condition, convert(branch.high)),
                                                   normalized.conjunction(normalized.negate(condition), convert(branch.low)));
        converted.emplace(id, result); return result;
    };
    for (auto& connection : connections) connection.guard = convert(connection.guard);
    for (auto& effect : effects) effect.guard = convert(effect.guard);
    for (auto& [name, declaration] : declarations) declaration.guard = convert(declaration.guard);
    for (auto& [name, memory] : memories) for (auto& port : memory.ports) port.guard = convert(port.guard);
    guards = std::move(normalized);
}
void Semantics::analyze() {
    for (const auto& port : body.interface.ports) {
        auto type = declaredType(body.arena, port.type, port.range);
        declarations.emplace(std::string(port.name), Declaration{std::string(port.name), type, port.range, port.direction == LeafDirection::Input ? 1 : 2, {}, true});
        declarationOrder.emplace_back(port.name);
    }
    collect(body.statements); inferMemoryPorts(body.statements);
    for (const auto& [name, entry] : memoryPorts) {
        const auto& port = memories.at(entry.first).ports[entry.second];
        declarations.at(name).flow = (port.read ? 1 : 0) | (port.write ? 2 : 0);
    }
    statements(body.statements, 1); solve(); normalizeGuards();
    for (const auto& [name, declaration] : declarations) for (const auto& leaf : leaves(declaration.type, declaration.flow)) total(leaf);
}
int64_t Semantics::width(TypePtr type) const { return widths_.at(type->width).value; }
TypeKind Semantics::resolvedKind(TypePtr type) const {
    return type->kind == TypeKind::Reset ? (resetFlags(type->reset) == 2 ? TypeKind::AsyncReset : TypeKind::UInt) : type->kind;
}
std::vector<Leaf> Semantics::leaves(TypePtr type, int flow) const {
    std::vector<Leaf> result;
    std::function<void(TypePtr, Leaf)> visit = [&](TypePtr current, Leaf leaf) {
        if (current->kind == TypeKind::Probe || current->kind == TypeKind::RWProbe) {
            leaf.probe = current->kind;
            if (layerEnabled(current->layer)) visit(current->element, std::move(leaf));
        }
        else if (current->kind == TypeKind::Vector) { leaf.shape.push_back(current->extent); visit(current->element, std::move(leaf)); }
        else if (current->kind == TypeKind::Bundle) {
            for (const auto& field : current->fields) { auto child = leaf; child.fields.push_back(field.name); if (field.flip) child.flow = reversed(child.flow); visit(field.type, std::move(child)); }
        } else { leaf.type = current; result.push_back(std::move(leaf)); }
    };
    visit(std::move(type), Leaf{nullptr, {}, {}, flow}); return result;
}
int64_t Semantics::total(const Leaf& leaf) const {
    int64_t bits = width(leaf.type);
    if (std::find(leaf.shape.begin(), leaf.shape.end(), 0) != leaf.shape.end()) return 0;
    for (const auto extent : leaf.shape) {
        if (extent && bits > kMaximumWidth / extent) throw Error(widths_[leaf.type->width].range, "FIRRTL Vector total width exceeds GRH range");
        bits *= extent;
    }
    return bits;
}
TypePtr Semantics::projected(TypePtr type, const std::vector<std::string>& fields) const {
    for (const auto& name : fields) {
        while (type->kind == TypeKind::Vector || type->kind == TypeKind::Probe || type->kind == TypeKind::RWProbe) type = type->element;
        const auto found = std::find_if(type->fields.begin(), type->fields.end(), [&](const auto& field) { return field.name == name; });
        if (found == type->fields.end()) throw std::logic_error("invalid FIRRTL leaf projection");
        type = found->type;
    }
    while (type->kind == TypeKind::Vector || type->kind == TypeKind::Probe || type->kind == TypeKind::RWProbe) type = type->element;
    return type;
}
std::optional<int64_t> Semantics::staticIndex(ExprId id) const {
    const auto& expr = body.arena.expressions[id];
    if (expr.kind == ExprKind::Integer) return integer64({TokenKind::Number, expr.literal, expr.range}, true);
    if (expr.kind == ExprKind::UInt) {
        const auto value = literal(expr.literal, false, expr.range);
        if (value.bits.size() > 62) return {};
        int64_t result = 0; for (const char bit : value.bits) result = result * 2 + bit - '0'; return result;
    }
    return {};
}
} // namespace wolvrix::lib::ingest::firrtl::detail
