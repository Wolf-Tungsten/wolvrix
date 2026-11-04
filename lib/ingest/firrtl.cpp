#include "ingest/firrtl.hpp"
#include "firrtl/semantic.hpp"
#include "firrtl/annotations.hpp"
#include "mixed.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <thread>
#include <unordered_set>

namespace wolvrix::lib::ingest {
namespace {
IntegerParameter integerParameter(std::string_view literal) {
    const bool negative = literal.front() == '-';
    if (negative) literal.remove_prefix(1);
    std::vector<unsigned> bits(1, 0);
    for (const char digit : literal) {
        unsigned carry = digit - '0';
        for (auto& bit : bits) { const unsigned value = bit * 10 + carry; bit = value & 1; carry = value >> 1; }
        while (carry) { bits.push_back(carry & 1); carry >>= 1; }
    }
    bits.resize(std::max<std::size_t>(32, bits.size() + 1), 0);
    if (negative) {
        unsigned carry = 1;
        for (auto& bit : bits) { const unsigned value = (bit ^ 1) + carry; bit = value & 1; carry = value >> 1; }
    }
    while (bits.size() > 32 && bits.back() == bits[bits.size() - 2]) bits.pop_back();
    IntegerParameter result; result.width = static_cast<uint32_t>(bits.size()); result.isSigned = true;
    for (auto bit = bits.rbegin(); bit != bits.rend(); ++bit) result.bits += char('0' + *bit);
    return result;
}

void flattenPort(const firrtl::Source& source, firrtl::TypeId id, PortLeafSignature leaf,
                 std::vector<PortLeafSignature>& leaves, std::unordered_set<std::string_view>& aliases,
                 std::size_t depth = 0) {
    if (depth > 256) throw std::invalid_argument("FIRRTL port type nesting or alias cycle exceeds limit");
    const auto& type = source.arena().types.at(id);
    leaf.isConst |= type.constant;
    using Kind = firrtl::TypeKind;
    if (type.kind == Kind::Alias) {
        const auto found = std::find_if(source.aliases().begin(), source.aliases().end(),
                                        [&](const auto& alias) { return alias.name == type.name; });
        if (found == source.aliases().end() || !aliases.insert(type.name).second)
            throw std::invalid_argument("unknown or cyclic FIRRTL type alias '" + std::string(type.name) + "'");
        flattenPort(source, found->type, std::move(leaf), leaves, aliases, depth + 1); aliases.erase(type.name); return;
    }
    if (type.kind == Kind::Vector) {
        leaf.shape.push_back(type.extent);
        leaf.shapeFieldDepths.push_back(leaf.fieldPath.size());
        if (!type.extent) leaf.requiresTypeResolution = true;
        flattenPort(source, type.element, std::move(leaf), leaves, aliases, depth + 1); return;
    }
    if (type.kind == Kind::Bundle) {
        for (const auto& field : type.fields) {
            auto child = leaf; child.fieldPath.emplace_back(field.name); child.name += "__DOT__" + std::string(field.name);
            if (field.flip) child.direction = child.direction == LeafDirection::Input ? LeafDirection::Output : LeafDirection::Input;
            flattenPort(source, field.type, std::move(child), leaves, aliases, depth + 1);
        }
        return;
    }
    if (type.kind == Kind::Probe || type.kind == Kind::RWProbe) {
        leaf.sourceType = type.kind == Kind::Probe ? "Probe" : "RWProbe"; leaf.requiresTypeResolution = true;
        flattenPort(source, type.element, std::move(leaf), leaves, aliases, depth + 1); return;
    }
    std::string kind;
    switch (type.kind) {
    case Kind::UInt: kind = "UInt"; break; case Kind::SInt: kind = "SInt"; break;
    case Kind::Clock: kind = "Clock"; break; case Kind::Reset: kind = "Reset"; break;
    case Kind::AsyncReset: kind = "AsyncReset"; break; case Kind::Analog: kind = "Analog"; break;
    case Kind::Integer: kind = "Integer"; break; case Kind::String: kind = "String"; break;
    case Kind::Bool: kind = "Bool"; break; case Kind::Double: kind = "Double"; break;
    case Kind::Path: kind = "Path"; break; case Kind::AnyRef: kind = "AnyRef"; break;
    case Kind::Inst: kind = "Inst"; break; case Kind::List: kind = "List"; break;
    case Kind::Enum: kind = "Enum"; break;
    default: throw std::invalid_argument("unsupported FIRRTL port type");
    }
    leaf.sourceType = leaf.sourceType.empty() ? kind : leaf.sourceType + "<" + kind + ">";
    leaf.isSigned = type.kind == Kind::SInt;
    if (type.kind == Kind::UInt || type.kind == Kind::SInt || type.kind == Kind::Analog) {
        if (type.width > std::numeric_limits<int32_t>::max()) throw std::invalid_argument("FIRRTL port width exceeds GRH range");
        leaf.elementWidth = static_cast<int32_t>(type.width);
        leaf.requiresTypeResolution |= type.width <= 0 || type.kind == Kind::Analog;
    } else if (type.kind == Kind::Clock || type.kind == Kind::Reset || type.kind == Kind::AsyncReset) {
        leaf.elementWidth = 1; leaf.requiresTypeResolution |= type.kind == Kind::Reset;
    } else {
        leaf.requiresTypeResolution = true;
        if (type.kind == Kind::String) leaf.valueType = grh::ValueType::String;
        else if (type.kind == Kind::Double) leaf.valueType = grh::ValueType::Real;
    }
    leaves.push_back(std::move(leaf));
}

} // namespace

ModuleSignature describeFirrtlModule(const firrtl::Source& source, const firrtl::Module& module) {
    ModuleSignature result;
    result.language = SourceLanguage::Firrtl; result.sourceUnit = source.unit();
    result.name = module.name; result.implementationName = module.implementation;
    result.external = module.kind == firrtl::ModuleKind::External || module.kind == firrtl::ModuleKind::Intrinsic ||
                      module.kind == firrtl::ModuleKind::ExternalClass;
    result.location = source.position(module.range);
    for (const auto& parameter : module.parameters) {
        ModuleParameter value; value.name = parameter.name;
        if (parameter.kind == firrtl::LiteralKind::Integer) value.value = integerParameter(parameter.value);
        else if (parameter.kind == firrtl::LiteralKind::RawString) value.value = RawStringParameter{firrtl::decodeString(parameter.value)};
        else value.value = firrtl::decodeString(parameter.value);
        result.parameters.push_back(std::move(value));
    }
    std::unordered_set<std::string_view> aliases;
    std::unordered_set<std::string> names;
    for (const auto& port : module.ports) {
        PortLeafSignature leaf; leaf.name = port.name; leaf.fieldPath.emplace_back(port.name); leaf.direction = port.direction;
        flattenPort(source, port.type, std::move(leaf), result.ports, aliases);
    }
    for (const auto& leaf : result.ports)
        if (!names.insert(leaf.name).second) throw std::invalid_argument("flattened FIRRTL port name collision '" + leaf.name + "'");
    return result;
}

InterfaceScanResult scanFirrtlInterfaces(const IngestRequest& request) {
    InterfaceScanResult result;
    if (request.sources.empty()) {
        result.diagnostics.push_back({IngestSeverity::Error, "FIRRTL interface scan requires an input source", {}, "ingest-firrtl"});
        return result;
    }
    std::unordered_set<std::string> units;
    for (const auto& input : request.sources) {
        if (input.language != SourceLanguage::Firrtl) {
            result.diagnostics.push_back({IngestSeverity::Error, "mixed-language interface scanning is not supported yet", {}, "ingest-firrtl"});
            continue;
        }
        const auto unit = input.unit.empty() ? (input.path.is_absolute() ? input.path.filename().generic_string() : input.path.generic_string()) : input.unit;
        if (!units.insert(unit).second) {
            result.diagnostics.push_back({IngestSeverity::Error, "duplicate FIRRTL source unit '" + unit + "'", {}, "ingest-firrtl"}); continue;
        }
        const auto path = input.path.is_absolute() || request.inputRoot.empty() ? input.path : request.inputRoot / input.path;
        auto indexed = firrtl::indexFile(path, unit, request.firrtl);
        result.diagnostics.insert(result.diagnostics.end(), indexed.diagnostics.begin(), indexed.diagnostics.end());
        if (!indexed.success()) continue;
        result.sourceOwners.push_back(indexed.source);
        // Parse each body once during the production interface scan.  The
        // former call to validate() parsed every module only to discard its
        // AST and then lowering parsed reachable modules again.  Keep the
        // same source-order diagnostics while avoiding the separate census
        // pass; the explicit validate() API remains available to callers
        // that need construct statistics.
        const auto modules = indexed.source->modules();
        std::vector<firrtl::detail::SharedModuleResult> bodies(modules.size());
        std::vector<std::exception_ptr> failures(modules.size());
        std::vector<std::size_t> jobs(modules.size());
        std::iota(jobs.begin(), jobs.end(), 0);
        std::stable_sort(jobs.begin(), jobs.end(), [&](std::size_t left, std::size_t right) {
            return modules[left].range.end - modules[left].range.begin > modules[right].range.end - modules[right].range.begin;
        });
        std::atomic<std::size_t> next{0};
        const auto parse = [&] {
            for (;;) {
                const auto job = next.fetch_add(1, std::memory_order_relaxed);
                if (job >= jobs.size()) return;
                const auto index = jobs[job];
                try {
                    bodies[index] = firrtl::detail::parseSharedModule(indexed.source, index);
                    const auto kind = modules[index].kind;
                    if (bodies[index].success() && (kind == firrtl::ModuleKind::Module || kind == firrtl::ModuleKind::Class)) {
                        // Semantic failures remain inside the summary until a
                        // reachable module's interface is evaluated.
                        firrtl::detail::sharedInterfaceSummary(indexed.source, index, request.firrtl);
                    }
                } catch (...) { failures[index] = std::current_exception(); }
            }
        };
        const auto threads = std::min<std::size_t>(std::max<std::size_t>(1, request.firrtl.lowerThreads), jobs.size());
        if (threads <= 1) parse();
        else {
            std::vector<std::jthread> workers;
            workers.reserve(threads);
            for (std::size_t worker = 0; worker < threads; ++worker) workers.emplace_back(parse);
        }
        bool parsed = true;
        for (std::size_t index = 0; index < modules.size(); ++index) {
            if (failures[index]) std::rethrow_exception(failures[index]);
            const auto& body = bodies[index];
            result.diagnostics.insert(result.diagnostics.end(), body.diagnostics.begin(), body.diagnostics.end());
            if (!body.success()) parsed = false;
        }
        if (!parsed) continue;
        for (const auto& module : indexed.source->modules()) {
            try { result.modules.push_back(describeFirrtlModule(*indexed.source, module)); }
            catch (const std::exception& error) {
                result.diagnostics.push_back({IngestSeverity::Error, error.what(), std::string(module.name), "ingest-firrtl",
                                              std::string(module.name), indexed.source->position(module.range)});
            }
        }
    }
    result.success = std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                                  [](const auto& diagnostic) { return diagnostic.kind == IngestSeverity::Error; });
    return result;
}

IngestResult readFirrtl(const IngestRequest& request) {
    return readSources(request);
}

} // namespace wolvrix::lib::ingest
