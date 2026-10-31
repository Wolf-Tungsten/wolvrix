#include "semantic.hpp"

#include <algorithm>
#include <bit>

namespace wolvrix::lib::ingest::firrtl::detail {
namespace {
bool memoryData(TypePtr type) {
    if (type->kind == TypeKind::Vector) return memoryData(type->element);
    if (type->kind == TypeKind::Bundle)
        return std::all_of(type->fields.begin(), type->fields.end(), [](const auto& field) { return !field.flip && memoryData(field.type); });
    return type->kind == TypeKind::UInt || type->kind == TypeKind::SInt;
}
bool constData(TypePtr type) {
    if (type->constant) return true;
    if (type->element && constData(type->element)) return true;
    return std::any_of(type->fields.begin(), type->fields.end(), [](const auto& field) { return constData(field.type); });
}
TypePtr bundle() { auto type = std::make_shared<SemanticType>(); type->kind = TypeKind::Bundle; return type; }
int64_t property(const MemoryField& field) {
    return integer64({TokenKind::Number, field.value, field.range}, true);
}
}

void Semantics::collectMemory(const Statement& statement) {
    Memory memory; memory.name = statement.name; memory.range = statement.range;
    memory.data = declaredType(body.arena, statement.type, statement.range);
    memory.standard = statement.kind == StmtKind::Memory;
    if (memory.standard) {
        for (const auto& field : statement.memory) {
            if (field.name == "depth") memory.depth = property(field);
            else if (field.name == "read-latency") memory.readLatency = property(field);
            else if (field.name == "write-latency") memory.writeLatency = property(field);
            else if (field.name == "read-under-write") memory.readUnderWrite = field.value;
            else if (field.name == "reader" || field.name == "writer" || field.name == "readwriter")
                memory.ports.push_back({std::string(field.value), field.name != "writer", field.name != "reader", false, {}, {}, 1, field.range});
        }
    } else {
        if (memory.data->kind != TypeKind::Vector) throw Error(statement.range, "CHIRRTL memory requires an array type");
        memory.depth = memory.data->extent; memory.data = memory.data->element;
        memory.readLatency = statement.kind == StmtKind::SMem ? 1 : 0;
        if (!statement.target.empty()) memory.readUnderWrite = statement.target;
    }
    if (!memory.depth || !memory.writeLatency) throw Error(statement.range, "memory depth and write latency must be positive");
    if (memory.readLatency > 1024 || memory.writeLatency > 1024) throw Error(statement.range, "memory latency exceeds frontend resource limit (1024)");
    if (!memoryData(memory.data)) throw Error(statement.range, "memory data must have a passive storable type");
    if (memory.standard) {
        auto type = bundle();
        const auto addressWidth = static_cast<int64_t>(std::bit_width(static_cast<uint64_t>(memory.depth - 1)));
        const auto mask = [&](auto&& self, TypePtr data) -> TypePtr {
            if (data->kind == TypeKind::Vector) { auto result = std::make_shared<SemanticType>(*data); result->element = self(self, data->element); return result; }
            if (data->kind == TypeKind::Bundle) { auto result = bundle(); for (const auto& field : data->fields) result->fields.push_back({field.name, false, self(self, field.type)}); return result; }
            return ground(TypeKind::UInt, 1, statement.range);
        };
        for (const auto& port : memory.ports) {
            if (constData(memory.data) && port.write) throw Error(port.range, "constant memory cannot have write ports");
            auto fields = bundle();
            fields->fields = {{"addr", false, ground(TypeKind::UInt, addressWidth, port.range)},
                              {"en", false, ground(TypeKind::UInt, 1, port.range)},
                              {"clk", false, ground(TypeKind::Clock, 1, port.range)}};
            if (port.read) fields->fields.push_back({port.write ? "rdata" : "data", true, memory.data});
            if (port.write) {
                fields->fields.push_back({port.read ? "wdata" : "data", false, memory.data});
                fields->fields.push_back({port.read ? "wmask" : "mask", false, mask(mask, memory.data)});
                if (port.read) fields->fields.push_back({"wmode", false, ground(TypeKind::UInt, 1, port.range)});
            }
            type->fields.push_back({port.name, true, fields});
        }
        declarations.emplace(memory.name, Declaration{memory.name, type, statement.range, 1});
        declarations.at(memory.name).layer = activeLayer_;
        declarationOrder.push_back(memory.name);
    }
    memories.emplace(memory.name, std::move(memory));
}

void Semantics::collectMemoryPort(const Statement& statement) {
    const auto& access = body.arena.expressions[statement.operands[0]];
    const auto& target = body.arena.expressions[access.operands[0]];
    const auto found = memories.find(std::string(target.name));
    if (found == memories.end() || found->second.standard) throw Error(statement.range, "mport target is not a CHIRRTL memory");
    auto& memory = found->second;
    MemoryPort port; port.name = statement.name; port.range = statement.range;
    port.read = statement.target == "read" || statement.target == "rdwr";
    port.write = statement.target == "write" || statement.target == "rdwr"; port.inferred = statement.target == "infer";
    if (constData(memory.data) && port.write) throw Error(statement.range, "constant memory cannot have write ports");
    port.address = access.operands[1]; port.clock = statement.operands[1];
    memoryPorts.emplace(port.name, std::pair{memory.name, memory.ports.size()});
    declarations.emplace(port.name, Declaration{port.name, memory.data, statement.range, port.inferred ? 3 : (port.read ? 1 : 0) | (port.write ? 2 : 0)});
    declarations.at(port.name).layer = activeLayer_;
    declarationOrder.push_back(port.name); memory.ports.push_back(std::move(port));
}

void Semantics::inferMemoryPorts(std::span<const StmtId> statements) {
    const auto use = [&](auto&& self, ExprId id, bool write) -> void {
        const auto& expr = body.arena.expressions[id];
        if (expr.kind == ExprKind::Reference) {
            if (const auto found = memoryPorts.find(std::string(expr.name)); found != memoryPorts.end()) {
                auto& port = memories.at(found->second.first).ports[found->second.second];
                if (port.inferred) { if (write) port.write = true; else port.read = true; }
            }
        } else {
            for (std::size_t i = 0; i < expr.operands.size(); ++i)
                self(self, expr.operands[i], write && i == 0 && (expr.kind == ExprKind::Index || expr.kind == ExprKind::Field));
        }
    };
    for (const auto id : statements) {
        if (!activeStatements.contains(id)) continue;
        const auto& statement = body.arena.statements[id];
        for (std::size_t i = 0; i < statement.operands.size(); ++i)
            use(use, statement.operands[i], i == 0 && (statement.kind == StmtKind::Connect || statement.kind == StmtKind::Invalidate));
        inferMemoryPorts(statement.body); inferMemoryPorts(statement.otherwise);
    }
    for (const auto& [name, memory] : memories)
        if (constData(memory.data)) for (const auto& port : memory.ports)
            if (port.write) throw Error(port.range, "constant memory cannot have write ports");
}
} // namespace wolvrix::lib::ingest::firrtl::detail
