#include "core/op_descriptor.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace wolvrix::lib::grh {
namespace {
    struct Registry {
        std::unordered_map<OperationKind, const OpDescriptor*> byKind;
        std::unordered_map<std::string_view, const OpDescriptor*> byName;
        std::unordered_map<std::string_view, uint32_t> versions;
    };

    Registry& registry()
    {
        static Registry value;
        return value;
    }

    const auto& builtinDescriptors()
    {
        static const auto table = [] {
            std::array<OpDescriptor, static_cast<std::size_t>(OperationKind::kXMRWrite) + 1> result{};
            for (std::size_t i = 0; i < result.size(); ++i)
            {
                const auto kind = static_cast<OperationKind>(i);
                OpEffect effect = OpEffect::Pure;
                switch (kind)
                {
                case OperationKind::kRegister:
                case OperationKind::kLatch:
                case OperationKind::kDpicImport: effect = OpEffect::Declaration; break;
                case OperationKind::kRegisterReadPort:
                case OperationKind::kRegisterReadArrayPort:
                case OperationKind::kLatchReadPort: effect = OpEffect::ReadState; break;
                case OperationKind::kRegisterWritePort:
                case OperationKind::kRegisterWriteMaskPort:
                case OperationKind::kRegisterWriteArrayPort:
                case OperationKind::kRegisterWriteArrayMaskPort:
                case OperationKind::kRegisterFillPort:
                case OperationKind::kLatchWritePort: effect = OpEffect::WriteState; break;
                case OperationKind::kSystemTask:
                case OperationKind::kSystemFunction:
                case OperationKind::kDpicCall:
                case OperationKind::kInstance:
                case OperationKind::kBlackbox:
                case OperationKind::kXMRRead:
                case OperationKind::kXMRWrite: effect = OpEffect::External; break;
                default: break;
                }
                result[i] = {kind, toString(kind), "grh", 2, effect,
                             effect == OpEffect::Pure || effect == OpEffect::ReadState,
                             effect == OpEffect::Pure || effect == OpEffect::ReadState};
            }
            return result;
        }();
        return table;
    }
}

void registerOpDescriptors(std::span<const OpDescriptor> descriptors)
{
    auto& entries = registry();
    for (const OpDescriptor& descriptor : descriptors)
    {
        if (static_cast<unsigned>(descriptor.kind) <= static_cast<unsigned>(OperationKind::kXMRWrite) ||
            descriptor.name.empty() || descriptor.dialect.empty() || descriptor.dialect == "grh" ||
            descriptor.version == 0 || entries.byKind.contains(descriptor.kind) ||
            entries.byName.contains(descriptor.name))
            throw std::logic_error("Invalid or duplicate dialect descriptor");
        const auto [version, inserted] = entries.versions.emplace(descriptor.dialect, descriptor.version);
        if (!inserted && version->second != descriptor.version)
            throw std::logic_error("Conflicting dialect versions");
        entries.byKind.emplace(descriptor.kind, &descriptor);
        entries.byName.emplace(descriptor.name, &descriptor);
    }
}

const OpDescriptor* findOpDescriptor(OperationKind kind) noexcept
{
    const auto index = static_cast<std::size_t>(kind);
    if (index <= static_cast<std::size_t>(OperationKind::kXMRWrite))
        return &builtinDescriptors()[index];
    const auto& entries = registry().byKind;
    const auto it = entries.find(kind);
    return it == entries.end() ? nullptr : it->second;
}

std::optional<OperationKind> findDialectOperation(std::string_view name) noexcept
{
    const auto& entries = registry().byName;
    const auto it = entries.find(name);
    if (it == entries.end()) return std::nullopt;
    return it->second->kind;
}

std::optional<uint32_t> dialectVersion(std::string_view dialect) noexcept
{
    if (dialect == "grh") return 2;
    const auto& versions = registry().versions;
    const auto it = versions.find(dialect);
    if (it == versions.end()) return std::nullopt;
    return it->second;
}

std::map<std::string, uint32_t> requiredDialects(const Graph& graph)
{
    std::map<std::string, uint32_t> result{{"grh", 2}};
    for (const OperationId operation : graph.operations())
    {
        const auto* descriptor = findOpDescriptor(graph.opKind(operation));
        if (!descriptor) throw std::runtime_error("Cannot store an unknown operation kind");
        result.emplace(descriptor->dialect, descriptor->version);
    }
    return result;
}

std::string verifyOpDescriptor(const Graph& graph, OperationId operation)
{
    const auto* descriptor = findOpDescriptor(graph.opKind(operation));
    if (!descriptor) return "Unknown operation kind";
    const auto operands = graph.opOperands(operation);
    const auto results = graph.opResults(operation);
    if (operands.size() < descriptor->minOperands || operands.size() > descriptor->maxOperands ||
        results.size() < descriptor->minResults || results.size() > descriptor->maxResults)
        return std::string(descriptor->name) + ": invalid operand/result count";
    const auto attrs = graph.opAttrs(operation);
    for (const AttrSchema& schema : descriptor->attributes)
    {
        const auto it = std::find_if(attrs.begin(), attrs.end(),
                                      [&](const AttrKV& entry) { return entry.key == schema.name; });
        if (it == attrs.end())
        {
            if (schema.required) return "Missing attribute " + std::string(schema.name);
        }
        else if (it->value.index() != schema.variantIndex)
            return "Wrong attribute type for " + std::string(schema.name);
    }
    try
    {
        return descriptor->verify ? descriptor->verify(graph, operation) : std::string();
    }
    catch (const std::exception& error)
    {
        return std::string(descriptor->name) + ": " + error.what();
    }
}

OpEffect operationEffect(const Graph& graph, OperationId operation)
{
    const auto* descriptor = findOpDescriptor(graph.opKind(operation));
    if (!descriptor) throw std::runtime_error("Effects are unknown for an unregistered operation");
    return descriptor->effectFor ? descriptor->effectFor(graph, operation) : descriptor->effect;
}

bool hasDialectOperations(const Graph& graph)
{
    if (graph.metadata("grhsim.stage")) return true;
    for (const OperationId operation : graph.operations())
    {
        const auto* descriptor = findOpDescriptor(graph.opKind(operation));
        if (!descriptor || descriptor->dialect != "grh") return true;
    }
    return false;
}

bool canCseOperation(const Graph& graph, OperationId operation)
{
    const auto* descriptor = findOpDescriptor(graph.opKind(operation));
    if (!descriptor || !descriptor->canCse) return false;
    const auto effect = operationEffect(graph, operation);
    return effect == OpEffect::Pure || effect == OpEffect::ReadState;
}

bool canCloneOperation(const Graph& graph, OperationId operation)
{
    const auto* descriptor = findOpDescriptor(graph.opKind(operation));
    if (!descriptor || !descriptor->canClone) return false;
    const auto effect = operationEffect(graph, operation);
    return effect == OpEffect::Pure || effect == OpEffect::ReadState;
}

} // namespace wolvrix::lib::grh
