#include "core/ingest.hpp"

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Scope.h"
#include "slang/ast/Statement.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/numeric/SVInt.h"
#include "slang/numeric/ConstantValue.h"

#include "slang/ast/symbols/AttributeSymbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/ValueSymbol.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/text/SourceManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <future>
#include <iterator>
#include <limits>
#include <sstream>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>

namespace wolvrix::lib::ingest {

static std::string procKindText(ProcKind kind);

// Visitor to force resolution of all expression types before parallel conversion.
// This ensures thread-safe access to the AST by resolving all lazy bindings
// that might not be triggered by DiagnosticVisitor (which uses VisitStatements=false
// and VisitExpressions=false).
struct ExpressionPrebindVisitor : public slang::ast::ASTVisitor<ExpressionPrebindVisitor, true, true> {
    template<typename T>
    void handle(const T& expr) {
        if constexpr (std::is_base_of_v<slang::ast::Expression, T>) {
            // Force type resolution by accessing the type
            (void)expr.type;
        }
        if constexpr (std::is_base_of_v<slang::ast::Symbol, T>) {
            // Force declared type resolution
            if (auto declaredType = expr.getDeclaredType()) {
                (void)declaredType->getType();
            }
        }
        visitDefault(expr);
    }
};

class InstanceRegistry {
public:
    bool trySchedule(const PlanKey& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [it, inserted] = entries_.try_emplace(key);
        if (inserted)
        {
            auto promise = std::make_shared<std::promise<void>>();
            it->second.promise = promise;
            it->second.future = promise->get_future().share();
            return true;
        }
        return false;
    }

    void markReady(const PlanKey& key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end())
        {
            return;
        }
        if (it->second.readySignaled || !it->second.promise)
        {
            return;
        }
        it->second.promise->set_value();
        it->second.readySignaled = true;
    }

    std::shared_future<void> futureFor(const PlanKey& key) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end())
        {
            return {};
        }
        return it->second.future;
    }

private:
    struct Entry {
        std::shared_ptr<std::promise<void>> promise;
        std::shared_future<void> future;
        bool readySignaled = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<PlanKey, Entry, PlanKeyHash> entries_;
};

namespace {

std::string toLowerCopy(std::string_view text)
{
    std::string lowered;
    lowered.reserve(text.size());
    for (unsigned char raw : text)
    {
        lowered.push_back(static_cast<char>(std::tolower(raw)));
    }
    return lowered;
}

std::string normalizeSystemTaskName(std::string_view name)
{
    if (!name.empty() && name.front() == '$')
    {
        name.remove_prefix(1);
    }
    return toLowerCopy(name);
}

std::string sanitizeParamToken(std::string_view text, bool allowLeadingDigit = false)
{
    std::string result;
    result.reserve(text.size());
    bool lastUnderscore = false;

    for (unsigned char raw : text)
    {
        char ch = static_cast<char>(raw);
        if (std::isalnum(raw) || ch == '_' || ch == '$')
        {
            result.push_back(ch);
            lastUnderscore = false;
            continue;
        }

        if (lastUnderscore)
        {
            continue;
        }

        result.push_back('_');
        lastUnderscore = true;
    }

    if (!result.empty() && result.back() == '_')
    {
        result.pop_back();
    }

    if (!allowLeadingDigit && !result.empty() &&
        std::isdigit(static_cast<unsigned char>(result.front())))
    {
        result.insert(result.begin(), '_');
    }

    return result;
}

using ConvertClock = std::chrono::steady_clock;

std::string formatDuration(ConvertClock::duration duration)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    if (ms > 0)
    {
        return std::to_string(ms) + "ms";
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    if (us > 0)
    {
        return std::to_string(us) + "us";
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    return std::to_string(ns) + "ns";
}

void logPassStart(Logger* logger, bool timingEnabled, std::string_view passName,
                  std::string_view moduleName)
{
    if (!timingEnabled || !logger || !logger->enabled(LogLevel::Trace, "timing"))
    {
        return;
    }
    std::string message;
    message.reserve(passName.size() + moduleName.size() + 32);
    message.append(passName);
    if (!moduleName.empty())
    {
        message.append(" (");
        message.append(moduleName);
        message.append(")");
    }
    message.append(" start");
    logger->log(LogLevel::Trace, "timing", message);
}

void logPassTiming(Logger* logger, bool timingEnabled, std::string_view passName,
                   std::string_view moduleName, ConvertClock::duration duration)
{
    if (!timingEnabled || !logger || !logger->enabled(LogLevel::Trace, "timing"))
    {
        return;
    }
    std::string message;
    message.reserve(passName.size() + moduleName.size() + 32);
    message.append(passName);
    if (!moduleName.empty())
    {
        message.append(" (");
        message.append(moduleName);
        message.append(")");
    }
    message.append(" took ");
    message.append(formatDuration(duration));
    logger->log(LogLevel::Trace, "timing", message);
}

std::string parameterValueToString(const slang::ConstantValue& value)
{
    if (value.bad())
    {
        return "bad";
    }

    std::string sanitized = sanitizeParamToken(value.toString(), true);
    if (sanitized.empty())
    {
        sanitized = "value";
    }
    return sanitized;
}

std::string formatIntegerLiteral(const slang::SVInt& value)
{
    return value.toString(slang::SVInt::MAX_BITS);
}

std::optional<std::string> formatHierarchicalReference(
    const slang::ast::HierarchicalValueExpression& expr)
{
    const auto& ref = expr.ref;
    if (ref.path.empty())
    {
        return std::nullopt;
    }
    std::string result;
    for (const auto& elem : ref.path)
    {
        if (const auto* name = std::get_if<std::string_view>(&elem.selector))
        {
            if (!result.empty())
            {
                result.push_back('.');
            }
            result.append(*name);
            continue;
        }
        if (const auto* index = std::get_if<int32_t>(&elem.selector))
        {
            result.push_back('[');
            result.append(std::to_string(*index));
            result.push_back(']');
            continue;
        }
        if (const auto* range = std::get_if<std::pair<int32_t, int32_t>>(&elem.selector))
        {
            result.push_back('[');
            result.append(std::to_string(range->first));
            result.push_back(':');
            result.append(std::to_string(range->second));
            result.push_back(']');
            continue;
        }
    }
    if (result.empty())
    {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> extractXmrPath(const slang::ast::HierarchicalValueExpression& expr)
{
    if (auto name = formatHierarchicalReference(expr))
    {
        return name;
    }
    if (!expr.symbol.name.empty())
    {
        return std::string(expr.symbol.name);
    }
    return std::nullopt;
}

std::optional<int64_t> evalConstInt(const ModulePlan& plan, const LoweringPlan& lowering,
                                    ExprNodeId id);

std::string typeParameterToString(const slang::ast::TypeParameterSymbol& param)
{
    return sanitizeParamToken(param.getTypeAlias().toString());
}

void reportUnsupportedPort(const slang::ast::Symbol& symbol, std::string_view description,
                           ConvertDiagnostics* diagnostics)
{
    if (!diagnostics)
    {
        return;
    }
    std::string message = "Unsupported port form: ";
    message.append(description);
    diagnostics->error(symbol, message);
}

bool hasBlackboxAttribute(const slang::ast::InstanceBodySymbol& body)
{
    auto checkAttrs = [](std::span<const slang::ast::AttributeSymbol* const> attrs) {
        for (const slang::ast::AttributeSymbol* attr : attrs)
        {
            if (!attr)
            {
                continue;
            }
            const std::string lowered = toLowerCopy(attr->name);
            if (lowered == "blackbox" || lowered == "black_box" || lowered == "syn_black_box")
            {
                return true;
            }
        }
        return false;
    };

    slang::ast::Compilation& compilation = body.getCompilation();
    if (checkAttrs(compilation.getAttributes(body.getDefinition())))
    {
        return true;
    }
    return checkAttrs(compilation.getAttributes(body));
}

bool hasBlackboxImplementation(const slang::ast::InstanceBodySymbol& body)
{
    for (const slang::ast::Symbol& member : body.members())
    {
        if (member.as_if<slang::ast::ContinuousAssignSymbol>() ||
            member.as_if<slang::ast::ProceduralBlockSymbol>() ||
            member.as_if<slang::ast::InstanceSymbol>() ||
            member.as_if<slang::ast::InstanceArraySymbol>() ||
            member.as_if<slang::ast::GenerateBlockSymbol>() ||
            member.as_if<slang::ast::GenerateBlockArraySymbol>())
        {
            return true;
        }
    }
    return false;
}

bool isBlackboxBody(const slang::ast::InstanceBodySymbol& body, ConvertDiagnostics* diagnostics)
{
    const bool explicitAttribute = hasBlackboxAttribute(body);
    const bool hasImplementation = hasBlackboxImplementation(body);
    if (explicitAttribute && hasImplementation && diagnostics)
    {
        diagnostics->error(body.getDefinition(),
                           "Module marked as blackbox but contains implementation; treating as "
                           "normal module body");
    }
    return !hasImplementation;
}

struct ParameterSnapshot {
    std::string signature;
    std::vector<InstanceParameter> parameters;
};

ParameterSnapshot snapshotParameters(const slang::ast::InstanceBodySymbol& body, ModulePlan* plan)
{
    ParameterSnapshot snapshot;
    for (const slang::ast::ParameterSymbolBase* paramBase : body.getParameters())
    {
        if (!paramBase || paramBase->isLocalParam())
        {
            continue;
        }

        const std::string_view name = paramBase->symbol.name;
        if (name.empty())
        {
            continue;
        }

        std::string value;
        if (const auto* valueParam = paramBase->symbol.as_if<slang::ast::ParameterSymbol>())
        {
            value = parameterValueToString(valueParam->getValue());
        }
        else if (const auto* typeParam =
                     paramBase->symbol.as_if<slang::ast::TypeParameterSymbol>())
        {
            value = typeParameterToString(*typeParam);
        }
        else
        {
            value = "unsupported_param";
        }

        if (value.empty())
        {
            continue;
        }

        if (!snapshot.signature.empty())
        {
            snapshot.signature.push_back(';');
        }
        snapshot.signature.append(name);
        snapshot.signature.push_back('=');
        snapshot.signature.append(value);

        if (plan)
        {
            InstanceParameter param;
            param.symbol = plan->symbolTable.intern(name);
            param.value = value;
            snapshot.parameters.push_back(std::move(param));
        }
    }

    return snapshot;
}

struct TypeResolution {
    int32_t width = 1;
    bool isSigned = false;
    wolvrix::lib::grh::ValueType valueType = wolvrix::lib::grh::ValueType::Logic;
    int64_t memoryRows = 0;
    std::vector<int32_t> packedDims;
    std::vector<UnpackedDimInfo> unpackedDims;
    bool hasUnpacked = false;
};

bool isFlattenedNetArray(const SignalInfo& signal)
{
    return signal.kind == SignalKind::Net && signal.memoryRows > 0;
}

int64_t flattenedNetWidth(const SignalInfo& signal)
{
    if (!isFlattenedNetArray(signal))
    {
        return signal.width;
    }
    const int64_t elementWidth = signal.width > 0 ? signal.width : 1;
    const int64_t rows = signal.memoryRows > 0 ? signal.memoryRows : 1;
    const int64_t maxWidth = static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    if (rows > 0 && elementWidth > maxWidth / rows)
    {
        return maxWidth;
    }
    const int64_t total = elementWidth * rows;
    return std::min<int64_t>(total, maxWidth);
}

int64_t flattenedAggregateWidth(const SignalInfo& signal)
{
    const int64_t elementWidth = signal.width > 0 ? signal.width : 1;
    const int64_t rows = signal.memoryRows > 0 ? signal.memoryRows : 1;
    const int64_t maxWidth = static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    if (rows > 0 && elementWidth > maxWidth / rows)
    {
        return maxWidth;
    }
    const int64_t total = elementWidth * rows;
    return std::min<int64_t>(total, maxWidth);
}

bool isPackedAggregateVariable(const SignalInfo& signal)
{
    return signal.kind != SignalKind::Net && signal.kind != SignalKind::Memory &&
           signal.memoryRows > 0 && signal.packedDims.empty();
}

int32_t clampWidth(uint64_t width, const slang::ast::Symbol& origin, ConvertDiagnostics* diagnostics,
                   std::string_view label)
{
    if (width == 0)
    {
        if (diagnostics)
        {
            std::string message(label);
            message.append(" has indeterminate width; treating as 1-bit placeholder");
            diagnostics->error(origin, std::move(message));
        }
        return 1;
    }

    constexpr uint64_t maxValue = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    if (width > maxValue)
    {
        if (diagnostics)
        {
            std::string message(label);
            message.append(" width exceeds GRH limit; clamping to int32_t::max");
            diagnostics->error(origin, std::move(message));
        }
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(width);
}

int32_t clampDim(uint64_t width, const slang::ast::Symbol& origin, ConvertDiagnostics* diagnostics,
                 std::string_view label)
{
    if (width == 0)
    {
        if (diagnostics)
        {
            std::string message(label);
            message.append(" must have positive extent; treating as 1");
            diagnostics->error(origin, std::move(message));
        }
        return 1;
    }

    constexpr uint64_t maxValue = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    if (width > maxValue)
    {
        if (diagnostics)
        {
            std::string message(label);
            message.append(" exceeds GRH limit; clamping to int32_t::max");
            diagnostics->error(origin, std::move(message));
        }
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(width);
}

uint64_t computeFixedWidth(const slang::ast::Type& type, const slang::ast::Symbol& origin,
                           ConvertDiagnostics* diagnostics)
{
    const uint64_t bitstreamWidth = type.getBitstreamWidth();
    if (bitstreamWidth > 0)
    {
        return bitstreamWidth;
    }

    if (type.hasFixedRange())
    {
        const uint64_t selectable = type.getSelectableWidth();
        if (selectable > 0)
        {
            return selectable;
        }
    }

    const slang::ast::Type& canonical = type.getCanonicalType();
    auto accumulateStruct = [&](const slang::ast::Scope& scope, bool isUnion) -> uint64_t {
        uint64_t total = 0;
        uint64_t maxWidth = 0;
        for (const auto& field : scope.membersOfType<slang::ast::FieldSymbol>())
        {
            const uint64_t fieldWidth = computeFixedWidth(field.getType(), field, diagnostics);
            if (fieldWidth == 0)
            {
                continue;
            }
            total += fieldWidth;
            if (fieldWidth > maxWidth)
            {
                maxWidth = fieldWidth;
            }
        }
        return isUnion ? maxWidth : total;
    };

    switch (canonical.kind)
    {
    case slang::ast::SymbolKind::PackedArrayType: {
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        const uint64_t elementWidth = computeFixedWidth(packed.elementType, origin, diagnostics);
        if (elementWidth == 0)
        {
            return 0;
        }
        const uint64_t elements = packed.range.fullWidth();
        return elementWidth * elements;
    }
    case slang::ast::SymbolKind::FixedSizeUnpackedArrayType: {
        const auto& unpacked = canonical.as<slang::ast::FixedSizeUnpackedArrayType>();
        const uint64_t elementWidth = computeFixedWidth(unpacked.elementType, origin, diagnostics);
        if (elementWidth == 0)
        {
            return 0;
        }
        const uint64_t elements = unpacked.range.fullWidth();
        return elementWidth * elements;
    }
    case slang::ast::SymbolKind::PackedStructType:
        return accumulateStruct(canonical.as<slang::ast::Scope>(), false);
    case slang::ast::SymbolKind::UnpackedStructType:
        return accumulateStruct(canonical.as<slang::ast::Scope>(), false);
    case slang::ast::SymbolKind::PackedUnionType:
        return accumulateStruct(canonical.as<slang::ast::Scope>(), true);
    case slang::ast::SymbolKind::UnpackedUnionType:
        return accumulateStruct(canonical.as<slang::ast::Scope>(), true);
    case slang::ast::SymbolKind::TypeAlias: {
        const auto& alias = canonical.as<slang::ast::TypeAliasType>();
        return computeFixedWidth(alias.targetType.getType(), origin, diagnostics);
    }
    default:
        break;
    }
    return 0;
}

void collectPackedDims(const slang::ast::Type& type, const slang::ast::Symbol& origin,
                       ConvertDiagnostics* diagnostics, std::vector<int32_t>& out)
{
    const slang::ast::Type* current = &type;
    while (current)
    {
        const slang::ast::Type& canonical = current->getCanonicalType();
        if (canonical.kind == slang::ast::SymbolKind::TypeAlias)
        {
            current = &canonical.as<slang::ast::TypeAliasType>().targetType.getType();
            continue;
        }
        if (canonical.kind == slang::ast::SymbolKind::PackedArrayType)
        {
            const auto& packed = canonical.as<slang::ast::PackedArrayType>();
            const uint64_t extent = packed.range.fullWidth();
            out.push_back(clampDim(extent, origin, diagnostics, "Packed array dimension"));
            current = &packed.elementType;
            continue;
        }
        break;
    }
}

std::optional<std::pair<int32_t, int64_t>>
analyzePackedElementMemoryType(const slang::ast::Type& type,
                               const slang::ast::Symbol& origin,
                               ConvertDiagnostics* diagnostics)
{
    const slang::ast::Type* current = &type;
    while (current)
    {
        const slang::ast::Type& canonical = current->getCanonicalType();
        if (canonical.kind == slang::ast::SymbolKind::TypeAlias)
        {
            current = &canonical.as<slang::ast::TypeAliasType>().targetType.getType();
            continue;
        }
        if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
        {
            return std::nullopt;
        }
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        const slang::ast::Type& elementType = packed.elementType.getCanonicalType();
        if (elementType.kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType)
        {
            return std::nullopt;
        }
        const uint64_t rowsRaw = packed.range.fullWidth();
        const uint64_t widthRaw = computeFixedWidth(elementType, origin, diagnostics);
        if (rowsRaw <= 1 || widthRaw <= 1)
        {
            return std::nullopt;
        }
        return std::make_pair(
            clampWidth(widthRaw, origin, diagnostics, "Packed array element"),
            static_cast<int64_t>(clampDim(rowsRaw, origin, diagnostics,
                                          "Packed array memory dimension")));
    }
    return std::nullopt;
}

bool containsUnpackedDims(const slang::ast::Type& type, const slang::ast::Symbol& origin,
                          ConvertDiagnostics* diagnostics)
{
    const slang::ast::Type* current = &type;
    while (current)
    {
        const slang::ast::Type& canonical = current->getCanonicalType();
        switch (canonical.kind)
        {
        case slang::ast::SymbolKind::TypeAlias:
            current = &canonical.as<slang::ast::TypeAliasType>().targetType.getType();
            continue;
        case slang::ast::SymbolKind::PackedArrayType:
            current = &canonical.as<slang::ast::PackedArrayType>().elementType;
            continue;
        case slang::ast::SymbolKind::FixedSizeUnpackedArrayType:
            return true;
        case slang::ast::SymbolKind::DynamicArrayType:
        case slang::ast::SymbolKind::AssociativeArrayType:
        case slang::ast::SymbolKind::QueueType:
            if (diagnostics)
            {
                diagnostics->error(origin,
                                   "Unsupported unpacked array kind on port declaration");
            }
            return true;
        default:
            break;
        }
        break;
    }
    return false;
}

wolvrix::lib::grh::ValueType classifyValueType(const slang::ast::Type& type)
{
    if (type.isString())
    {
        return wolvrix::lib::grh::ValueType::String;
    }
    if (type.isFloating())
    {
        return wolvrix::lib::grh::ValueType::Real;
    }
    return wolvrix::lib::grh::ValueType::Logic;
}

TypeResolution analyzeSignalType(const slang::ast::Type& type, const slang::ast::Symbol& origin,
                                 ConvertDiagnostics* diagnostics)
{
    TypeResolution info{};
    info.valueType = classifyValueType(type);
    if (info.valueType != wolvrix::lib::grh::ValueType::Logic)
    {
        if (containsUnpackedDims(type, origin, diagnostics))
        {
            if (diagnostics)
            {
                diagnostics->error(origin,
                                   "Unsupported array dimensions on real/string signal");
            }
        }
        std::vector<int32_t> packedDims;
        collectPackedDims(type, origin, diagnostics, packedDims);
        if (!packedDims.empty() && diagnostics)
        {
            diagnostics->error(origin,
                               "Unsupported packed dimensions on real/string signal");
        }
        info.width = 1;
        info.isSigned = false;
        info.memoryRows = 0;
        return info;
    }
    const slang::ast::Type* current = &type;
    int64_t rows = 1;

    while (current)
    {
        const slang::ast::Type& canonical = current->getCanonicalType();
        if (canonical.kind == slang::ast::SymbolKind::TypeAlias)
        {
            current = &canonical.as<slang::ast::TypeAliasType>().targetType.getType();
            continue;
        }
        if (canonical.kind == slang::ast::SymbolKind::FixedSizeUnpackedArrayType)
        {
            info.hasUnpacked = true;
            const auto& unpacked = canonical.as<slang::ast::FixedSizeUnpackedArrayType>();
            const slang::ConstantRange range = unpacked.range;
            uint64_t extent = unpacked.range.fullWidth();
            if (extent == 0)
            {
                if (diagnostics)
                {
                    diagnostics->error(origin,
                                       "Unpacked array dimension must have positive extent");
                }
                extent = 1;
            }
            UnpackedDimInfo dim;
            dim.extent = clampDim(extent, origin, diagnostics, "Unpacked array dimension");
            dim.left = range.left;
            dim.right = range.right;
            info.unpackedDims.push_back(dim);

            constexpr uint64_t maxRows =
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
            uint64_t total = static_cast<uint64_t>(rows);
            if (extent != 0 && total > maxRows / extent)
            {
                if (diagnostics)
                {
                    diagnostics->error(origin,
                                       "Memory row count exceeds GRH limit; clamping to "
                                       "int64_t::max");
                }
                rows = std::numeric_limits<int64_t>::max();
            }
            else
            {
                total *= extent;
                rows = static_cast<int64_t>(total);
            }

            current = &unpacked.elementType;
            continue;
        }

        if (canonical.kind == slang::ast::SymbolKind::DynamicArrayType ||
            canonical.kind == slang::ast::SymbolKind::AssociativeArrayType ||
            canonical.kind == slang::ast::SymbolKind::QueueType)
        {
            if (diagnostics)
            {
                diagnostics->error(origin,
                                   "Unsupported unpacked array kind on signal declaration");
            }
            break;
        }
        break;
    }

    if (!current)
    {
        return info;
    }

    collectPackedDims(*current, origin, diagnostics, info.packedDims);
    const uint64_t width = computeFixedWidth(*current, origin, diagnostics);
    info.width = clampWidth(width, origin, diagnostics, "Signal");
    info.isSigned = current->isSigned();
    info.valueType = wolvrix::lib::grh::ValueType::Logic;
    if (info.hasUnpacked)
    {
        info.memoryRows = rows > 0 ? rows : 1;
    }
    return info;
}

TypeResolution analyzePortType(const slang::ast::Type& type, const slang::ast::Symbol& origin,
                               ConvertDiagnostics* diagnostics)
{
    TypeResolution info{};
    info.valueType = classifyValueType(type);
    if (info.valueType != wolvrix::lib::grh::ValueType::Logic)
    {
        if (containsUnpackedDims(type, origin, diagnostics) && diagnostics)
        {
            diagnostics->error(origin,
                               "Unsupported array dimensions on real/string port");
        }
        info.width = 1;
        info.isSigned = false;
        return info;
    }
    if (containsUnpackedDims(type, origin, diagnostics))
    {
        if (diagnostics)
        {
            diagnostics->warn(origin,
                              "Unpacked array port flattened; array dimensions are ignored");
        }
    }
    const uint64_t width = computeFixedWidth(type, origin, diagnostics);
    info.width = clampWidth(width, origin, diagnostics, "Port");
    info.isSigned = type.isSigned();
    return info;
}

const slang::ast::TimingControl* findTimingControl(const slang::ast::Statement& stmt)
{
    if (const auto* timed = stmt.as_if<slang::ast::TimedStatement>())
    {
        return &timed->timing;
    }

    if (const auto* block = stmt.as_if<slang::ast::BlockStatement>())
    {
        return findTimingControl(block->body);
    }

    if (const auto* list = stmt.as_if<slang::ast::StatementList>())
    {
        for (const slang::ast::Statement* child : list->list)
        {
            if (!child)
            {
                continue;
            }
            if (const auto* timing = findTimingControl(*child))
            {
                return timing;
            }
        }
    }
    return nullptr;
}

bool isLevelSensitiveEventList(const slang::ast::TimingControl& timing)
{
    using slang::ast::TimingControlKind;
    switch (timing.kind)
    {
    case TimingControlKind::SignalEvent: {
        const auto& signal = timing.as<slang::ast::SignalEventControl>();
        return signal.edge == slang::ast::EdgeKind::None;
    }
    case TimingControlKind::EventList: {
        const auto& list = timing.as<slang::ast::EventListControl>();
        bool hasSignal = false;
        for (const slang::ast::TimingControl* ctrl : list.events)
        {
            if (!ctrl)
            {
                continue;
            }
            if (!isLevelSensitiveEventList(*ctrl))
            {
                return false;
            }
            hasSignal = true;
        }
        return hasSignal;
    }
    case TimingControlKind::RepeatedEvent:
        return isLevelSensitiveEventList(
            timing.as<slang::ast::RepeatedEventControl>().event);
    default:
        return false;
    }
}

bool containsEdgeSensitiveEvent(const slang::ast::TimingControl& timing)
{
    using slang::ast::TimingControlKind;
    switch (timing.kind)
    {
    case TimingControlKind::SignalEvent: {
        const auto& signal = timing.as<slang::ast::SignalEventControl>();
        return signal.edge != slang::ast::EdgeKind::None;
    }
    case TimingControlKind::EventList: {
        const auto& list = timing.as<slang::ast::EventListControl>();
        for (const slang::ast::TimingControl* ctrl : list.events)
        {
            if (!ctrl)
            {
                continue;
            }
            if (containsEdgeSensitiveEvent(*ctrl))
            {
                return true;
            }
        }
        return false;
    }
    case TimingControlKind::RepeatedEvent:
        return containsEdgeSensitiveEvent(
            timing.as<slang::ast::RepeatedEventControl>().event);
    default:
        return false;
    }
}

ControlDomain classifyProceduralBlock(const slang::ast::ProceduralBlockSymbol& block)
{
    using slang::ast::ProceduralBlockKind;
    switch (block.procedureKind)
    {
    case ProceduralBlockKind::AlwaysComb:
        return ControlDomain::Combinational;
    case ProceduralBlockKind::AlwaysLatch:
        return ControlDomain::Latch;
    case ProceduralBlockKind::AlwaysFF:
    case ProceduralBlockKind::Initial:
    case ProceduralBlockKind::Final:
        return ControlDomain::Sequential;
    case ProceduralBlockKind::Always: {
        const slang::ast::TimingControl* timing = findTimingControl(block.getBody());
        if (!timing || timing->kind == slang::ast::TimingControlKind::ImplicitEvent)
        {
            return ControlDomain::Combinational;
        }
        if (containsEdgeSensitiveEvent(*timing))
        {
            return ControlDomain::Sequential;
        }
        if (isLevelSensitiveEventList(*timing))
        {
            return ControlDomain::Combinational;
        }
        return ControlDomain::Unknown;
    }
    default:
        break;
    }
    return ControlDomain::Unknown;
}

ProcKind mapProcKind(slang::ast::ProceduralBlockKind kind)
{
    using slang::ast::ProceduralBlockKind;
    switch (kind)
    {
    case ProceduralBlockKind::Initial:
        return ProcKind::Initial;
    case ProceduralBlockKind::Final:
        return ProcKind::Final;
    case ProceduralBlockKind::AlwaysComb:
        return ProcKind::AlwaysComb;
    case ProceduralBlockKind::AlwaysLatch:
        return ProcKind::AlwaysLatch;
    case ProceduralBlockKind::AlwaysFF:
        return ProcKind::AlwaysFF;
    case ProceduralBlockKind::Always:
        return ProcKind::Always;
    default:
        break;
    }
    return ProcKind::Unknown;
}

std::optional<wolvrix::lib::grh::OperationKind> mapUnaryOp(slang::ast::UnaryOperator op)
{
    switch (op)
    {
    case slang::ast::UnaryOperator::BitwiseNot:
        return wolvrix::lib::grh::OperationKind::kNot;
    case slang::ast::UnaryOperator::LogicalNot:
        return wolvrix::lib::grh::OperationKind::kLogicNot;
    case slang::ast::UnaryOperator::BitwiseAnd:
        return wolvrix::lib::grh::OperationKind::kReduceAnd;
    case slang::ast::UnaryOperator::BitwiseOr:
        return wolvrix::lib::grh::OperationKind::kReduceOr;
    case slang::ast::UnaryOperator::BitwiseXor:
        return wolvrix::lib::grh::OperationKind::kReduceXor;
    case slang::ast::UnaryOperator::BitwiseNand:
        return wolvrix::lib::grh::OperationKind::kReduceNand;
    case slang::ast::UnaryOperator::BitwiseNor:
        return wolvrix::lib::grh::OperationKind::kReduceNor;
    case slang::ast::UnaryOperator::BitwiseXnor:
        return wolvrix::lib::grh::OperationKind::kReduceXnor;
    default:
        break;
    }
    return std::nullopt;
}

std::optional<wolvrix::lib::grh::OperationKind> mapBinaryOp(slang::ast::BinaryOperator op)
{
    switch (op)
    {
    case slang::ast::BinaryOperator::Add:
        return wolvrix::lib::grh::OperationKind::kAdd;
    case slang::ast::BinaryOperator::Subtract:
        return wolvrix::lib::grh::OperationKind::kSub;
    case slang::ast::BinaryOperator::Multiply:
        return wolvrix::lib::grh::OperationKind::kMul;
    case slang::ast::BinaryOperator::Divide:
        return wolvrix::lib::grh::OperationKind::kDiv;
    case slang::ast::BinaryOperator::Mod:
        return wolvrix::lib::grh::OperationKind::kMod;
    case slang::ast::BinaryOperator::BinaryAnd:
        return wolvrix::lib::grh::OperationKind::kAnd;
    case slang::ast::BinaryOperator::BinaryOr:
        return wolvrix::lib::grh::OperationKind::kOr;
    case slang::ast::BinaryOperator::BinaryXor:
        return wolvrix::lib::grh::OperationKind::kXor;
    case slang::ast::BinaryOperator::BinaryXnor:
        return wolvrix::lib::grh::OperationKind::kXnor;
    case slang::ast::BinaryOperator::Equality:
        return wolvrix::lib::grh::OperationKind::kEq;
    case slang::ast::BinaryOperator::CaseEquality:
        return wolvrix::lib::grh::OperationKind::kCaseEq;
    case slang::ast::BinaryOperator::WildcardEquality:
        return wolvrix::lib::grh::OperationKind::kWildcardEq;
    case slang::ast::BinaryOperator::Inequality:
        return wolvrix::lib::grh::OperationKind::kNe;
    case slang::ast::BinaryOperator::CaseInequality:
        return wolvrix::lib::grh::OperationKind::kCaseNe;
    case slang::ast::BinaryOperator::WildcardInequality:
        return wolvrix::lib::grh::OperationKind::kWildcardNe;
    case slang::ast::BinaryOperator::GreaterThanEqual:
        return wolvrix::lib::grh::OperationKind::kGe;
    case slang::ast::BinaryOperator::GreaterThan:
        return wolvrix::lib::grh::OperationKind::kGt;
    case slang::ast::BinaryOperator::LessThanEqual:
        return wolvrix::lib::grh::OperationKind::kLe;
    case slang::ast::BinaryOperator::LessThan:
        return wolvrix::lib::grh::OperationKind::kLt;
    case slang::ast::BinaryOperator::LogicalAnd:
        return wolvrix::lib::grh::OperationKind::kLogicAnd;
    case slang::ast::BinaryOperator::LogicalOr:
        return wolvrix::lib::grh::OperationKind::kLogicOr;
    case slang::ast::BinaryOperator::LogicalShiftLeft:
    case slang::ast::BinaryOperator::ArithmeticShiftLeft:
        return wolvrix::lib::grh::OperationKind::kShl;
    case slang::ast::BinaryOperator::LogicalShiftRight:
        return wolvrix::lib::grh::OperationKind::kLShr;
    case slang::ast::BinaryOperator::ArithmeticShiftRight:
        return wolvrix::lib::grh::OperationKind::kAShr;
    default:
        break;
    }
    return std::nullopt;
}

PlanSymbolId resolveSimpleSymbolForPlan(const slang::ast::Expression& expr,
                                        const ModulePlan& plan);
const SignalInfo* findSignalBySymbol(const ModulePlan& plan, PlanSymbolId symbol);
PlanSymbolId makeInternalPlanValueSymbol(ModulePlan& plan);

// Resolves a slang symbol to its plan signal symbol. Signals declared inside
// generate scopes are interned under scope-qualified names, so bare-name text
// lookup would miss them (or silently hit a same-named top-level symbol); the
// identity map recorded by collectSignals resolves them exactly.
PlanSymbolId lookupPlanSignal(const ModulePlan& plan, const slang::ast::Symbol& symbol)
{
    if (auto it = plan.signalBySlangSymbol.find(&symbol);
        it != plan.signalBySlangSymbol.end())
    {
        return it->second;
    }
    if (symbol.name.empty())
    {
        return {};
    }
    return plan.symbolTable.lookup(symbol.name);
}

struct StmtLowererState {
    class AssignmentExprVisitor;

    enum class LoopControlResult { None, Break, Continue, Unsupported };
    enum class CaseLoweringMode { TwoState, FourState };

    struct CaseTwoStateAnalysis {
        bool hasDefault = false;
        bool twoStateComplete = false;
        bool canLowerTwoState = true;
    };

    struct ForeachDimState {
        const slang::ast::ValueSymbol* loopVar = nullptr;
        int32_t start = 0;
        int32_t stop = 0;
        int32_t step = 1;
    };

    ModulePlan& plan;
    ConvertDiagnostics* diagnostics;
    LoweringPlan& lowering;
    std::unordered_map<const slang::ast::Expression*, ExprNodeId> lowered;
    int32_t widthContext = 0;
    uint32_t maxLoopIterations = 0;
    ControlDomain domain = ControlDomain::Unknown;
    std::vector<ExprNodeId> guardStack;
    std::vector<ExprNodeId> flowStack;
    std::vector<bool> caseCoverageStack;
    struct LoopFlowContext {
        ExprNodeId loopAlive = kInvalidPlanIndex;
    };
    struct LoopLocalScope {
        StmtLowererState& state;
        explicit LoopLocalScope(StmtLowererState& state,
                                std::vector<const slang::ast::ValueSymbol*> vars)
            : state(state)
        {
            state.pushLoopLocals(std::move(vars));
        }
        ~LoopLocalScope()
        {
            state.popLoopLocals();
        }
    };
    struct WidthContextScope {
        StmtLowererState& state;
        int32_t saved = 0;

        WidthContextScope(StmtLowererState& state, int32_t width)
            : state(state), saved(state.widthContext)
        {
            state.widthContext = width;
        }

        ~WidthContextScope()
        {
            state.widthContext = saved;
        }
    };
    struct EventContext {
        bool hasTiming = false;
        bool edgeSensitive = false;
        ProcKind procKind = ProcKind::Unknown;
        std::vector<EventEdge> edges;
        std::vector<ExprNodeId> operands;
    };
    bool suppressAssignmentWrites = false;
    std::vector<std::unordered_map<const slang::ast::ValueSymbol*, int64_t>>
        loopLocalStack;
    std::vector<std::vector<const slang::ast::ValueSymbol*>> loopVarStack;
    std::vector<std::unordered_map<PlanIndex, ExprNodeId>> proceduralValueStack;
    std::vector<std::unordered_map<const slang::ast::ValueSymbol*, PlanSymbolId>>
        proceduralLocalStack;
    std::unordered_map<PlanIndex, const slang::ast::Type*> proceduralLocalTypes;
    struct LValueTarget {
        PlanSymbolId target;
        std::vector<WriteSlice> slices;
        uint64_t width = 0;
        bool isXmr = false;
        std::string xmrPath;
        slang::SourceLocation location{};
    };
    struct LValueCompositeInfo {
        bool isComposite = false;
        bool reverseOrder = false;
    };
    struct AssignmentSuppressScope {
        StmtLowererState& state;
        bool saved = false;

        AssignmentSuppressScope(StmtLowererState& state, bool enable)
            : state(state), saved(state.suppressAssignmentWrites)
        {
            state.suppressAssignmentWrites = enable;
        }

        ~AssignmentSuppressScope()
        {
            state.suppressAssignmentWrites = saved;
        }
    };
    struct ProceduralLocalScope {
        StmtLowererState& state;
        bool active = false;

        explicit ProceduralLocalScope(StmtLowererState& state)
            : state(state), active(state.hasProceduralValues())
        {
            if (active)
            {
                state.pushProceduralLocalScope();
            }
        }

        ~ProceduralLocalScope()
        {
            if (active)
            {
                state.popProceduralLocalScope();
            }
        }
    };
    std::vector<LoopFlowContext> loopFlowStack;
    std::optional<std::string> loopControlFailure;
    EventContext eventContext;

    StmtLowererState(ModulePlan& plan, ConvertDiagnostics* diagnostics, LoweringPlan& lowering,
                     uint32_t maxLoopIterations)
        : plan(plan),
          diagnostics(diagnostics),
          lowering(lowering),
          maxLoopIterations(maxLoopIterations)
    {
    }

    const PortInfo* findPortBySymbol(PlanSymbolId symbol) const
    {
        if (!symbol.valid())
        {
            return nullptr;
        }
        for (const auto& port : plan.ports)
        {
            if (port.symbol.index == symbol.index)
            {
                return &port;
            }
        }
        return nullptr;
    }

    const PortInfo::InoutBinding* inoutBindingForSymbol(PlanSymbolId symbol) const
    {
        const PortInfo* port = findPortBySymbol(symbol);
        if (!port || port->direction != PortDirection::Inout || !port->inoutSymbol)
        {
            return nullptr;
        }
        return &(*port->inoutSymbol);
    }

    bool isInoutPortSymbol(PlanSymbolId symbol) const
    {
        return inoutBindingForSymbol(symbol) != nullptr;
    }

    const slang::ast::Expression* peelImplicit(const slang::ast::Expression& expr) const
    {
        const slang::ast::Expression* current = &expr;
        while (const auto* conversion = current->as_if<slang::ast::ConversionExpression>())
        {
            if (!conversion->isImplicit())
            {
                break;
            }
            current = &conversion->operand();
        }
        return current;
    }

    bool isAllZExpr(const slang::ast::Expression& expr) const
    {
        const slang::ast::Expression* current = peelImplicit(expr);
        if (!current)
        {
            return false;
        }
        if (const auto* conversion = current->as_if<slang::ast::ConversionExpression>())
        {
            return isAllZExpr(conversion->operand());
        }
        if (const auto* constant = current->getConstant())
        {
            if (constant->isInteger())
            {
                const slang::SVInt& literal = constant->integer();
                if (literal.getBitWidth() > 0 &&
                    literal.countZs() == literal.getBitWidth())
                {
                    return true;
                }
            }
        }
        if (const auto* literal = current->as_if<slang::ast::IntegerLiteral>())
        {
            const slang::SVInt& value = literal->getValue();
            if (value.getBitWidth() > 0 && value.countZs() == value.getBitWidth())
            {
                return true;
            }
        }
        if (const auto* literal =
                current->as_if<slang::ast::UnbasedUnsizedIntegerLiteral>())
        {
            const slang::SVInt& value = literal->getValue();
            if (value.getBitWidth() > 0 && value.countZs() == value.getBitWidth())
            {
                return true;
            }
        }
        if (const auto* repl = current->as_if<slang::ast::ReplicationExpression>())
        {
            if (!isAllZExpr(repl->concat()))
            {
                return false;
            }
            const slang::ConstantValue* countValue = repl->count().getConstant();
            if (!countValue || !countValue->isInteger() || countValue->hasUnknown())
            {
                return false;
            }
            auto count = countValue->integer().as<int64_t>();
            if (!count || *count <= 0)
            {
                return false;
            }
            return true;
        }
        if (const auto* concat = current->as_if<slang::ast::ConcatenationExpression>())
        {
            bool sawOperand = false;
            for (const slang::ast::Expression* operand : concat->operands())
            {
                if (!operand)
                {
                    continue;
                }
                sawOperand = true;
                if (!isAllZExpr(*operand))
                {
                    return false;
                }
            }
            return sawOperand;
        }
        if (const auto* stream =
                current->as_if<slang::ast::StreamingConcatenationExpression>())
        {
            if (stream->getSliceSize() != 0)
            {
                return false;
            }
            bool sawOperand = false;
            for (const auto& element : stream->streams())
            {
                if (element.withExpr || !element.operand)
                {
                    return false;
                }
                sawOperand = true;
                if (!isAllZExpr(*element.operand))
                {
                    return false;
                }
            }
            return sawOperand;
        }
        return false;
    }

    bool containsZLiteral(const slang::ast::Expression& expr) const
    {
        const slang::ast::Expression* current = peelImplicit(expr);
        if (!current)
        {
            return false;
        }
        if (const auto* conversion = current->as_if<slang::ast::ConversionExpression>())
        {
            return containsZLiteral(conversion->operand());
        }
        if (const auto* constant = current->getConstant())
        {
            if (constant->isInteger())
            {
                const slang::SVInt& literal = constant->integer();
                if (literal.countZs() > 0)
                {
                    return true;
                }
            }
        }
        if (const auto* literal = current->as_if<slang::ast::IntegerLiteral>())
        {
            if (literal->getValue().countZs() > 0)
            {
                return true;
            }
        }
        if (const auto* literal =
                current->as_if<slang::ast::UnbasedUnsizedIntegerLiteral>())
        {
            if (literal->getValue().countZs() > 0)
            {
                return true;
            }
        }
        if (const auto* repl = current->as_if<slang::ast::ReplicationExpression>())
        {
            return containsZLiteral(repl->concat());
        }
        if (const auto* concat = current->as_if<slang::ast::ConcatenationExpression>())
        {
            for (const slang::ast::Expression* operand : concat->operands())
            {
                if (!operand)
                {
                    continue;
                }
                if (containsZLiteral(*operand))
                {
                    return true;
                }
            }
            return false;
        }
        if (const auto* stream =
                current->as_if<slang::ast::StreamingConcatenationExpression>())
        {
            for (const auto& element : stream->streams())
            {
                if (element.operand && containsZLiteral(*element.operand))
                {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    bool handleInoutAssignment(const slang::ast::AssignmentExpression& expr,
                               const LValueTarget& target)
    {
        const PortInfo* port = findPortBySymbol(target.target);
        const PortInfo::InoutBinding* binding = inoutBindingForSymbol(target.target);
        if (!port || !binding)
        {
            return false;
        }

        auto clampWidth = [](uint64_t width) -> int32_t {
            const uint64_t maxValue =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            return width > maxValue ? std::numeric_limits<int32_t>::max()
                                    : static_cast<int32_t>(width);
        };

        int32_t portWidth = port->width;
        if (!target.slices.empty())
        {
            if (target.width > 0)
            {
                portWidth = clampWidth(target.width);
            }
            else if (expr.left().type)
            {
                const uint64_t widthRaw =
                    computeFixedWidth(*expr.left().type, *plan.body, diagnostics);
                if (widthRaw > 0)
                {
                    portWidth = clampWidth(widthRaw);
                }
            }
        }
        if (portWidth <= 0)
        {
            const uint64_t widthRaw = computeFixedWidth(*expr.left().type, *plan.body,
                                                        diagnostics);
            if (widthRaw > 0)
            {
                portWidth = clampWidth(widthRaw);
            }
        }
        if (portWidth <= 0)
        {
            portWidth = 1;
        }

        const slang::ast::Expression* rhs = peelImplicit(expr.right());
        if (!rhs)
        {
            return false;
        }

        ExprNodeId outExpr = kInvalidPlanIndex;
        ExprNodeId oeExpr = kInvalidPlanIndex;

        if (const auto* condExpr = rhs->as_if<slang::ast::ConditionalExpression>())
        {
            if (condExpr->conditions.size() == 1)
            {
                const slang::ast::Expression& condAst = *condExpr->conditions[0].expr;
                const slang::ast::Expression* left = peelImplicit(condExpr->left());
                const slang::ast::Expression* right = peelImplicit(condExpr->right());
                if (left && right)
                {
                    PlanSymbolId condSym = resolveSimpleSymbolForPlan(condAst, plan);
                    PlanSymbolId leftSym = resolveSimpleSymbolForPlan(*left, plan);
                    PlanSymbolId rightSym = resolveSimpleSymbolForPlan(*right, plan);
                    const bool condMatches =
                        condSym.valid() &&
                        condSym.index == binding->oeSymbol.index;
                    const bool leftMatches =
                        leftSym.valid() &&
                        leftSym.index == binding->outSymbol.index;
                    const bool rightMatches =
                        rightSym.valid() &&
                        rightSym.index == binding->outSymbol.index;
                    const bool leftHasZ = containsZLiteral(*left);
                    const bool rightHasZ = containsZLiteral(*right);
                    if (condMatches &&
                        ((leftMatches && rightHasZ) ||
                         (rightMatches && leftHasZ)))
                    {
                        return true;
                    }
                    const bool leftZ = isAllZExpr(*left);
                    const bool rightZ = isAllZExpr(*right);
                    if (leftZ != rightZ)
                    {
                        ExprNodeId condId = lowerExpression(condAst);
                        if (condId == kInvalidPlanIndex)
                        {
                            return false;
                        }
                        condId = makeLogicNot(makeLogicNot(condId, expr.sourceRange.start()),
                                              expr.sourceRange.start());
                        if (rightZ)
                        {
                            outExpr = lowerExpression(*left);
                            oeExpr = condId;
                        }
                        else
                        {
                            outExpr = lowerExpression(*right);
                            oeExpr = makeLogicNot(condId, expr.sourceRange.start());
                        }
                    }
                }
            }
        }

        if (outExpr == kInvalidPlanIndex || oeExpr == kInvalidPlanIndex)
        {
            if (isAllZExpr(*rhs))
            {
                outExpr = addConstantLiteral("0", expr.sourceRange.start());
                oeExpr = addConstantLiteral("1'b0", expr.sourceRange.start());
            }
            else
            {
                outExpr = lowerExpression(*rhs);
                oeExpr = addConstantLiteral("1'b1", expr.sourceRange.start());
            }
        }

        if (outExpr == kInvalidPlanIndex || oeExpr == kInvalidPlanIndex)
        {
            return false;
        }

        outExpr = resizeValueToWidth(outExpr, portWidth, port->isSigned,
                                     expr.sourceRange.start());
        int32_t oeWidth = 0;
        if (oeExpr < lowering.values.size())
        {
            oeWidth = lowering.values[oeExpr].widthHint;
        }
        if (oeWidth == 1 && portWidth > 1)
        {
            ExprNode countNode;
            countNode.kind = ExprNodeKind::Constant;
            countNode.literal = std::to_string(portWidth);
            countNode.location = expr.sourceRange.start();
            countNode.widthHint = 32;
            ExprNodeId countId = addNode(nullptr, std::move(countNode));
            ExprNode repNode;
            repNode.kind = ExprNodeKind::Operation;
            repNode.op = wolvrix::lib::grh::OperationKind::kReplicate;
            repNode.operands = {countId, oeExpr};
            repNode.location = expr.sourceRange.start();
            repNode.tempSymbol = makeTempSymbol();
            repNode.widthHint = portWidth;
            oeExpr = addNode(nullptr, std::move(repNode));
        }
        else
        {
            oeExpr = resizeValueToWidth(oeExpr, portWidth, false, expr.sourceRange.start());
        }

        ExprNodeId guard = currentGuard(expr.sourceRange.start());
        WriteIntent outIntent;
        outIntent.target = binding->outSymbol;
        outIntent.slices = target.slices;
        outIntent.value = outExpr;
        outIntent.guard = guard;
        outIntent.domain = domain;
        outIntent.isNonBlocking = expr.isNonBlocking();
        outIntent.location = expr.sourceRange.start();
        recordWriteIntent(std::move(outIntent));

        WriteIntent oeIntent;
        oeIntent.target = binding->oeSymbol;
        oeIntent.slices = target.slices;
        oeIntent.value = oeExpr;
        oeIntent.guard = guard;
        oeIntent.domain = domain;
        oeIntent.isNonBlocking = expr.isNonBlocking();
        oeIntent.location = expr.sourceRange.start();
        recordWriteIntent(std::move(oeIntent));

        return true;
    }

    void scanExpression(const slang::ast::Expression& expr)
    {
        if (!suppressAssignmentWrites)
        {
            if (const auto* assignment =
                    expr.as_if<slang::ast::AssignmentExpression>())
            {
                handleAssignment(*assignment);
                return;
            }
        }
        AssignmentExprVisitor visitor(*this);
        expr.visit(visitor);
    }

    void visitStatement(const slang::ast::Statement& stmt)
    {
        if (const auto* list = stmt.as_if<slang::ast::StatementList>())
        {
            for (const slang::ast::Statement* child : list->list)
            {
                if (!child)
                {
                    continue;
                }
                visitStatement(*child);
            }
            return;
        }
        if (const auto* block = stmt.as_if<slang::ast::BlockStatement>())
        {
            ProceduralLocalScope localScope(*this);
            handleProceduralBlockDeclarations(block->blockSymbol);
            visitStatement(block->body);
            return;
        }
        if (const auto* timed = stmt.as_if<slang::ast::TimedStatement>())
        {
            warnTimedStatement(*timed);
            visitStatement(timed->stmt);
            return;
        }
        if (const auto* wait = stmt.as_if<slang::ast::WaitStatement>())
        {
            warnWaitStatement(*wait);
            visitStatement(wait->stmt);
            return;
        }
        if (const auto* waitFork = stmt.as_if<slang::ast::WaitForkStatement>())
        {
            warnWaitForkStatement(*waitFork);
            return;
        }
        if (const auto* waitOrder = stmt.as_if<slang::ast::WaitOrderStatement>())
        {
            warnWaitOrderStatement(*waitOrder);
            if (waitOrder->ifTrue)
            {
                visitStatement(*waitOrder->ifTrue);
            }
            if (waitOrder->ifFalse)
            {
                visitStatement(*waitOrder->ifFalse);
            }
            return;
        }
        if (const auto* eventTrigger = stmt.as_if<slang::ast::EventTriggerStatement>())
        {
            warnEventTriggerStatement(*eventTrigger);
            return;
        }
        if (const auto* disableFork = stmt.as_if<slang::ast::DisableForkStatement>())
        {
            warnDisableForkStatement(*disableFork);
            return;
        }
        if (const auto* conditional = stmt.as_if<slang::ast::ConditionalStatement>())
        {
            visitConditional(*conditional);
            return;
        }
        if (const auto* caseStmt = stmt.as_if<slang::ast::CaseStatement>())
        {
            visitCase(*caseStmt);
            return;
        }
        if (const auto* patternCase = stmt.as_if<slang::ast::PatternCaseStatement>())
        {
            reportError(stmt, "Pattern case lowering is unsupported");
            return;
        }
        if (stmt.as_if<slang::ast::BreakStatement>())
        {
            if (handleLoopBreak(stmt))
            {
                return;
            }
            reportUnsupported(stmt, "Break statement lowering is unsupported");
            return;
        }
        if (stmt.as_if<slang::ast::ContinueStatement>())
        {
            if (handleLoopContinue(stmt))
            {
                return;
            }
            reportUnsupported(stmt, "Continue statement lowering is unsupported");
            return;
        }
        if (const auto* exprStmt = stmt.as_if<slang::ast::ExpressionStatement>())
        {
            if (handleExpressionStatement(exprStmt->expr))
            {
                return;
            }
            scanExpression(exprStmt->expr);
            return;
        }
        if (const auto* varDecl = stmt.as_if<slang::ast::VariableDeclStatement>())
        {
            handleProceduralLocalDeclaration(varDecl->symbol);
            return;
        }
        if (const auto* procAssign = stmt.as_if<slang::ast::ProceduralAssignStatement>())
        {
            scanExpression(procAssign->assignment);
            return;
        }
        const slang::ast::ForLoopStatement* forLoop =
            stmt.as_if<slang::ast::ForLoopStatement>();
        if (!forLoop && stmt.kind == slang::ast::StatementKind::ForLoop)
        {
            forLoop = &static_cast<const slang::ast::ForLoopStatement&>(stmt);
        }
        if (forLoop)
        {
            scanForLoopControl(*forLoop);
            const bool hasLoopControl = containsLoopControl(forLoop->body);
            if (hasLoopControl)
            {
                clearLoopControlFailure();
            }
            if (!tryUnrollFor(*forLoop))
            {
                if (hasLoopControl)
                {
                    reportLoopControlError(stmt,
                                           "For-loop with break/continue requires static unrolling");
                    return;
                }
                reportUnsupported(stmt, "For-loop lowering uses unconditional guards");
                visitStatement(forLoop->body);
            }
            return;
        }
        if (const auto* repeatLoop = stmt.as_if<slang::ast::RepeatLoopStatement>())
        {
            scanExpression(repeatLoop->count);
            const bool hasLoopControl = containsLoopControl(repeatLoop->body);
            if (hasLoopControl)
            {
                clearLoopControlFailure();
            }
            if (!tryUnrollRepeat(*repeatLoop))
            {
                if (hasLoopControl)
                {
                    reportLoopControlError(stmt,
                                           "Repeat-loop with break/continue requires static unrolling");
                    return;
                }
                reportUnsupported(stmt, "Repeat-loop lowering uses unconditional guards");
                visitStatement(repeatLoop->body);
            }
            return;
        }
        if (const auto* whileLoop = stmt.as_if<slang::ast::WhileLoopStatement>())
        {
            scanExpression(whileLoop->cond);
            const bool hasLoopControl = containsLoopControl(whileLoop->body);
            clearLoopControlFailure();
            if (!tryUnrollWhile(*whileLoop))
            {
                if (hasLoopControl)
                {
                    reportLoopControlError(stmt,
                                           "While-loop with break/continue requires static unrolling");
                    return;
                }
                reportLoopFailure(stmt, "While-loop lowering failed");
            }
            return;
        }
        if (const auto* doWhileLoop = stmt.as_if<slang::ast::DoWhileLoopStatement>())
        {
            scanExpression(doWhileLoop->cond);
            const bool hasLoopControl = containsLoopControl(doWhileLoop->body);
            clearLoopControlFailure();
            if (!tryUnrollDoWhile(*doWhileLoop))
            {
                if (hasLoopControl)
                {
                    reportLoopControlError(
                        stmt, "Do-while loop with break/continue requires static unrolling");
                    return;
                }
                reportLoopFailure(stmt, "Do-while loop lowering failed");
            }
            return;
        }
        if (const auto* foreverLoop = stmt.as_if<slang::ast::ForeverLoopStatement>())
        {
            const bool hasLoopControl = containsLoopControl(foreverLoop->body);
            clearLoopControlFailure();
            if (!tryUnrollForever(*foreverLoop))
            {
                if (hasLoopControl)
                {
                    reportLoopControlError(
                        stmt, "Forever-loop with break/continue requires static unrolling");
                    return;
                }
                reportLoopFailure(stmt, "Forever-loop lowering failed");
            }
            return;
        }
        if (const auto* foreachLoop = stmt.as_if<slang::ast::ForeachLoopStatement>())
        {
            scanExpression(foreachLoop->arrayRef);
            const bool hasLoopControl = containsLoopControl(foreachLoop->body);
            if (hasLoopControl)
            {
                clearLoopControlFailure();
            }
            if (!tryUnrollForeach(*foreachLoop))
            {
                if (hasLoopControl)
                {
                    reportLoopControlError(stmt,
                                           "Foreach-loop with break/continue requires static unrolling");
                    return;
                }
                reportUnsupported(stmt, "Foreach-loop lowering uses unconditional guards");
                visitStatement(foreachLoop->body);
            }
            return;
        }
        if (const auto* invalid = stmt.as_if<slang::ast::InvalidStatement>())
        {
            if (invalid->child)
            {
                visitStatement(*invalid->child);
            }
            return;
        }
        if (stmt.kind == slang::ast::StatementKind::Empty)
        {
            return;
        }
        reportUnsupported(stmt, "Unsupported statement kind");
    }

    void visitConditional(const slang::ast::ConditionalStatement& stmt)
    {
        if (stmt.conditions.empty())
        {
            reportUnsupported(stmt, "Conditional statement missing condition");
            return;
        }
        bool hasPattern = false;
        for (const auto& cond : stmt.conditions)
        {
            if (cond.pattern)
            {
                hasPattern = true;
            }
        }
        if (hasPattern)
        {
            reportError(stmt, "Patterned condition lowering is unsupported");
            return;
        }
        for (const auto& cond : stmt.conditions)
        {
            scanExpression(*cond.expr);
        }

        ExprNodeId combinedCond = kInvalidPlanIndex;
        for (const auto& cond : stmt.conditions)
        {
            ExprNodeId condId = lowerExpression(*cond.expr);
            if (condId == kInvalidPlanIndex)
            {
                reportUnsupported(stmt, "Failed to lower conditional guard");
                visitStatement(stmt.ifTrue);
                if (stmt.ifFalse)
                {
                    visitStatement(*stmt.ifFalse);
                }
                return;
            }
            if (combinedCond == kInvalidPlanIndex)
            {
                combinedCond = condId;
            }
            else
            {
                combinedCond =
                    makeLogicAnd(combinedCond, condId, stmt.sourceRange.start());
            }
        }

        ExprNodeId baseGuard = currentPathGuard();
        ExprNodeId trueGuard = combineGuard(baseGuard, combinedCond, stmt.sourceRange.start());
        if (!hasProceduralValues())
        {
            pushGuard(trueGuard);
            visitStatement(stmt.ifTrue);
            popGuard();

            if (stmt.ifFalse)
            {
                ExprNodeId notCond = makeLogicNot(combinedCond, stmt.sourceRange.start());
                ExprNodeId falseGuard = combineGuard(baseGuard, notCond, stmt.sourceRange.start());
                pushGuard(falseGuard);
                visitStatement(*stmt.ifFalse);
                popGuard();
            }
            return;
        }

        std::unordered_map<PlanIndex, ExprNodeId> baseValues = proceduralValueStack.back();
        auto runBranch = [&](ExprNodeId guard, const slang::ast::Statement& branch)
            -> std::unordered_map<PlanIndex, ExprNodeId> {
            proceduralValueStack.back() = baseValues;
            pushGuard(guard);
            visitStatement(branch);
            popGuard();
            return proceduralValueStack.back();
        };

        auto getValue = [&](const std::unordered_map<PlanIndex, ExprNodeId>& values,
                            PlanIndex index) -> ExprNodeId {
            auto it = values.find(index);
            if (it != values.end())
            {
                return it->second;
            }
            if (proceduralLocalTypes.find(index) != proceduralLocalTypes.end())
            {
                return makeProceduralLocalDefault(PlanSymbolId{index},
                                                  stmt.sourceRange.start());
            }
            ExprNode node;
            node.kind = ExprNodeKind::Symbol;
            node.symbol = PlanSymbolId{index};
            node.location = stmt.sourceRange.start();
            return addNode(nullptr, std::move(node));
        };

        std::unordered_map<PlanIndex, ExprNodeId> trueValues = runBranch(trueGuard, stmt.ifTrue);
        std::unordered_map<PlanIndex, ExprNodeId> falseValues = baseValues;
        if (stmt.ifFalse)
        {
            ExprNodeId notCond = makeLogicNot(combinedCond, stmt.sourceRange.start());
            ExprNodeId falseGuard = combineGuard(baseGuard, notCond, stmt.sourceRange.start());
            falseValues = runBranch(falseGuard, *stmt.ifFalse);
        }

        std::unordered_map<PlanIndex, ExprNodeId> merged = baseValues;
        std::unordered_set<PlanIndex> keys;
        keys.reserve(trueValues.size() + falseValues.size());
        for (const auto& entry : trueValues)
        {
            keys.insert(entry.first);
        }
        for (const auto& entry : falseValues)
        {
            keys.insert(entry.first);
        }
        for (PlanIndex index : keys)
        {
            ExprNodeId lhs = getValue(trueValues, index);
            ExprNodeId rhs = getValue(falseValues, index);
            if (lhs == rhs)
            {
                merged[index] = lhs;
                continue;
            }
            ExprNodeId mux = makeMux(combinedCond, lhs, rhs, stmt.sourceRange.start());
            if (mux != kInvalidPlanIndex)
            {
                merged[index] = mux;
            }
        }
        proceduralValueStack.back() = std::move(merged);
    }

    void visitCase(const slang::ast::CaseStatement& stmt)
    {
        scanExpression(stmt.expr);
        ExprNodeId control = lowerExpression(stmt.expr);
        if (control == kInvalidPlanIndex)
        {
            reportUnsupported(stmt, "Case control expression lowering failed");
            for (const auto& item : stmt.items)
            {
                if (item.stmt)
                {
                    visitStatement(*item.stmt);
                }
            }
            if (stmt.defaultCase)
            {
                visitStatement(*stmt.defaultCase);
            }
            return;
        }

        ExprNodeId baseGuard = currentPathGuard();
        ExprNodeId priorMatch = kInvalidPlanIndex;

        CaseLoweringMode mode = CaseLoweringMode::TwoState;
        bool caseCoversAll = false;
        bool warnFourState = false;
        if (stmt.condition != slang::ast::CaseStatementCondition::Inside)
        {
            CaseTwoStateAnalysis analysis = analyzeCaseTwoState(stmt);
            caseCoversAll = analysis.hasDefault || analysis.twoStateComplete;
            if (!caseCoversAll || !analysis.canLowerTwoState)
            {
                mode = CaseLoweringMode::FourState;
                warnFourState = true;
            }
        }
        else
        {
            caseCoversAll = stmt.defaultCase != nullptr;
        }

        if (warnFourState && diagnostics)
        {
            std::string originSymbol = describeFileLocation(stmt.sourceRange.start());
            diagnostics->warn(stmt.sourceRange.start(),
                              "Case lowered with 4-state semantics; may be unsynthesizable "
                              "(add default or full 2-state coverage)",
                              std::move(originSymbol));
        }

        const bool baseUnconditional = isUnconditionalGuard(baseGuard);
        pushCaseCoverage(baseUnconditional && caseCoversAll);

        for (const auto& item : stmt.items)
        {
            ExprNodeId itemMatch =
                buildCaseItemMatch(control, stmt.expr, stmt.condition, item.expressions,
                                   stmt.sourceRange.start(), mode);
            if (itemMatch == kInvalidPlanIndex)
            {
                reportUnsupported(stmt, "Failed to lower case item match");
                if (item.stmt)
                {
                    visitStatement(*item.stmt);
                }
                continue;
            }

            ExprNodeId itemGuard = combineGuard(baseGuard, itemMatch, stmt.sourceRange.start());
            if (priorMatch != kInvalidPlanIndex)
            {
                ExprNodeId notPrior = makeLogicNot(priorMatch, stmt.sourceRange.start());
                itemGuard = makeLogicAnd(itemGuard, notPrior, stmt.sourceRange.start());
            }

            pushGuard(itemGuard);
            if (item.stmt)
            {
                visitStatement(*item.stmt);
            }
            popGuard();

            if (priorMatch == kInvalidPlanIndex)
            {
                priorMatch = itemMatch;
            }
            else
            {
                priorMatch =
                    makeLogicOr(priorMatch, itemMatch, stmt.sourceRange.start());
            }
        }

        if (stmt.defaultCase)
        {
            ExprNodeId defaultGuard = baseGuard;
            if (priorMatch != kInvalidPlanIndex)
            {
                ExprNodeId notPrior = makeLogicNot(priorMatch, stmt.sourceRange.start());
                defaultGuard = combineGuard(baseGuard, notPrior, stmt.sourceRange.start());
            }
            pushGuard(defaultGuard);
            visitStatement(*stmt.defaultCase);
            popGuard();
        }
        popCaseCoverage();
    }

    void handleNetInitializer(const slang::ast::NetSymbol& net)
    {
        const slang::ast::Expression* init = net.getInitializer();
        if (!init || net.name.empty())
        {
            return;
        }
        const ControlDomain saved = domain;
        domain = ControlDomain::Combinational;
        int32_t targetWidth = 0;
        const uint64_t widthRaw =
            computeFixedWidth(net.getType(), net, diagnostics);
        if (widthRaw > 0)
        {
            const uint64_t maxValue =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            targetWidth = widthRaw > maxValue
                              ? std::numeric_limits<int32_t>::max()
                              : static_cast<int32_t>(widthRaw);
        }
        WidthContextScope widthScope(*this, targetWidth);
        ExprNodeId value = lowerExpression(*init);
        if (targetWidth > 0)
        {
            const bool signExtend = net.getType().isSigned();
            value = resizeValueToWidth(value, targetWidth, signExtend,
                                       init->sourceRange.start());
        }
        if (value != kInvalidPlanIndex)
        {
            PlanSymbolId target = lookupPlanSignal(plan, net);
            if (target.valid())
            {
                WriteIntent intent;
                intent.target = target;
                intent.value = value;
                intent.guard = currentGuard(init->sourceRange.start());
                intent.domain = domain;
                intent.isNonBlocking = false;
                intent.location = init->sourceRange.start();
                recordWriteIntent(std::move(intent));
            }
            else if (diagnostics)
            {
                diagnostics->error(net.location,
                                   "Unresolved target of net initializer");
            }
        }
        domain = saved;
    }

    void handleVariableInitializer(const slang::ast::VariableSymbol& variable)
    {
        const slang::ast::Expression* init = variable.getInitializer();
        if (!init || variable.name.empty())
        {
            return;
        }
        if (variable.getType().isEvent())
        {
            return;
        }

        PlanSymbolId target = lookupPlanSignal(plan, variable);
        if (!target.valid())
        {
            if (diagnostics)
            {
                diagnostics->error(variable.location,
                                   "Unresolved target of variable initializer");
            }
            return;
        }
        const SignalInfo* signal = findSignalBySymbol(plan, target);
        if (!signal || signal->memoryRows > 0 || signal->kind == SignalKind::Memory)
        {
            return;
        }

        std::string initValue = tryExtractInitValue(*init);
        if (initValue.empty())
        {
            if (diagnostics)
            {
                diagnostics->warn(
                    init->sourceRange.start(),
                    "Skipping variable initializer that cannot be represented as initValue");
            }
            return;
        }

        RegisterInit regInit;
        regInit.reg = target;
        regInit.initValue = std::move(initValue);
        regInit.location = init->sourceRange.start();
        lowering.registerInits.push_back(std::move(regInit));
    }

    void handleProceduralLocalDeclaration(const slang::ast::VariableSymbol& variable)
    {
        const slang::ast::Expression* init = variable.getInitializer();
        if (!hasProceduralValues())
        {
            if (init)
            {
                scanExpression(*init);
            }
            return;
        }
        if (variable.getType().isEvent())
        {
            return;
        }

        PlanSymbolId target = internProceduralLocal(variable);
        if (!target.valid())
        {
            return;
        }
        if (!init)
        {
            updateProceduralValue(
                target, makeProceduralLocalDefault(target, variable.location));
            return;
        }

        int32_t targetWidth = 0;
        const uint64_t widthRaw =
            computeFixedWidth(variable.getType(), variable, diagnostics);
        if (widthRaw > 0)
        {
            const uint64_t maxValue =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            targetWidth = widthRaw > maxValue
                              ? std::numeric_limits<int32_t>::max()
                              : static_cast<int32_t>(widthRaw);
        }

        ExprNodeId value = kInvalidPlanIndex;
        {
            WidthContextScope widthScope(*this, 0);
            value = lowerExpression(*init);
        }
        if (value == kInvalidPlanIndex)
        {
            return;
        }
        if (targetWidth > 0)
        {
            value = resizeValueToWidth(value, targetWidth, variable.getType().isSigned(),
                                       init->sourceRange.start());
        }
        if (value != kInvalidPlanIndex)
        {
            updateProceduralValue(target, value);
        }
    }

    void handleProceduralBlockDeclarations(const slang::ast::StatementBlockSymbol* block)
    {
        if (!block || !hasProceduralValues())
        {
            return;
        }
        for (const slang::ast::VariableSymbol& variable :
             block->membersOfType<slang::ast::VariableSymbol>())
        {
            handleProceduralLocalDeclaration(variable);
        }
    }

    void handleAssignment(const slang::ast::AssignmentExpression& expr)
    {
        std::vector<LValueTarget> targets;
        LValueCompositeInfo composite;
        if (!resolveLValueTargets(expr.left(), targets, composite))
        {
            return;
        }
        if (targets.empty())
        {
            reportUnsupported(expr, "Unsupported LHS in assignment");
            return;
        }

        // Try to record memory/register initialization in initial block
        // If handled, don't create write intent (init values are attributes, not writes)
        if (eventContext.procKind == ProcKind::Initial)
        {
            if (tryRecordMemoryInit(expr, targets))
            {
                return;
            }
            if (tryRecordRegisterInit(expr, targets))
            {
                return;
            }
        }

        if (targets.size() == 1 && !composite.isComposite &&
            isInoutPortSymbol(targets.front().target))
        {
            if (!handleInoutAssignment(expr, targets.front()))
            {
                reportUnsupported(expr, "Unsupported inout port assignment");
            }
            return;
        }
        if (const auto* assigned = resolveAssignedValueSymbol(expr.left()))
        {
            if (isLoopLocalSymbol(*assigned))
            {
                return;
            }
        }

        uint64_t totalWidth = 0;
        if (composite.isComposite || targets.size() > 1)
        {
            for (const auto& target : targets)
            {
                if (target.width == 0)
                {
                    reportUnsupported(expr, "Unsupported LHS width in assignment");
                    return;
                }
                totalWidth += target.width;
            }
            if (totalWidth == 0)
            {
                reportUnsupported(expr, "Unsupported LHS width in assignment");
                return;
            }
        }

        auto clampWidth = [](uint64_t width) -> int32_t {
            const uint64_t maxValue =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            return width > maxValue ? std::numeric_limits<int32_t>::max()
                                    : static_cast<int32_t>(width);
        };
        int32_t targetWidth = 0;
        if (composite.isComposite || targets.size() > 1)
        {
            targetWidth = clampWidth(totalWidth);
        }
        else if (targets.front().width > 0)
        {
            targetWidth = clampWidth(targets.front().width);
        }
        bool signExtend = false;
        if (!composite.isComposite && targets.size() == 1 && expr.left().type)
        {
            signExtend = expr.left().type->isSigned();
        }

        // Lower RHS without width context to preserve natural widths of sub-expressions
        // Width adjustment will be done explicitly at the assignment boundary
        ExprNodeId value = kInvalidPlanIndex;
        {
            WidthContextScope widthScope(*this, 0);
            if (expr.op)
            {
                const auto opKind = mapBinaryOp(*expr.op);
                if (!opKind)
                {
                    reportUnsupported(expr, "Unsupported compound assignment operator");
                    return;
                }
                ExprNodeId lhs = lowerExpression(expr.left());
                ExprNodeId rhs = lowerExpression(expr.right());
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return;
                }
                ExprNode node;
                node.kind = ExprNodeKind::Operation;
                node.op = *opKind;
                node.operands = {lhs, rhs};
                node.location = expr.sourceRange.start();
                node.tempSymbol = makeTempSymbol();
                value = addNode(nullptr, std::move(node));
            }
            else
            {
                value = lowerExpression(expr.right());
            }
        }
        if (value == kInvalidPlanIndex)
        {
            return;
        }
        if (targetWidth > 0)
        {
            value = resizeValueToWidth(value, targetWidth, signExtend,
                                       expr.sourceRange.start());
        }
        if (value == kInvalidPlanIndex)
        {
            return;
        }
        const bool assignsSingleProceduralLocal =
            targets.size() == 1 && !composite.isComposite &&
            !targets.front().isXmr && isProceduralLocalSymbol(targets.front().target);
        if (assignsSingleProceduralLocal)
        {
            if (expr.isNonBlocking())
            {
                reportUnsupported(expr, "Nonblocking procedural local assignment is unsupported");
                return;
            }
            if (targets.front().slices.empty())
            {
                updateProceduralValue(targets.front().target, value);
                return;
            }
            ExprNodeId updated =
                applyProceduralSliceUpdate(targets.front().target,
                                           targets.front().slices, value,
                                           expr.sourceRange.start());
            if (updated == kInvalidPlanIndex)
            {
                reportUnsupported(expr, "Unsupported procedural local slice assignment");
                return;
            }
            updateProceduralValue(targets.front().target, updated);
            return;
        }
        for (const auto& target : targets)
        {
            if (isProceduralLocalSymbol(target.target))
            {
                reportUnsupported(expr,
                                  "Composite procedural local assignment is unsupported");
                return;
            }
        }
        if (!expr.isNonBlocking() && hasProceduralValues() && targets.size() == 1 &&
            !composite.isComposite && !targets.front().isXmr)
        {
            if (targets.front().slices.empty())
            {
                updateProceduralValue(targets.front().target, value);
            }
            else
            {
                ExprNodeId updated = applyProceduralSliceUpdate(
                    targets.front().target, targets.front().slices, value,
                    expr.sourceRange.start());
                if (updated != kInvalidPlanIndex)
                {
                    updateProceduralValue(targets.front().target, updated);
                }
            }
        }

        ExprNodeId guard = currentGuard(expr.sourceRange.start());
        if (targets.size() == 1 && !composite.isComposite)
        {
            if (targets.front().isXmr && !targets.front().slices.empty())
            {
                reportUnsupported(expr, "Unsupported XMR slice write");
                return;
            }
            WriteIntent intent;
            intent.target = targets.front().target;
            intent.slices = std::move(targets.front().slices);
            intent.value = value;
            intent.guard = guard;
            intent.domain = domain;
            intent.isNonBlocking = expr.isNonBlocking();
            intent.isXmr = targets.front().isXmr;
            intent.xmrPath = std::move(targets.front().xmrPath);
            intent.location = expr.sourceRange.start();
            recordWriteIntent(std::move(intent));
            return;
        }

        if (totalWidth == 0)
        {
            reportUnsupported(expr, "Unsupported LHS width in assignment");
            return;
        }

        auto emitSliceWrite = [&](LValueTarget& target, uint64_t high, uint64_t low) {
            ExprNodeId sliceValue = makeRhsSlice(value, high, low, expr.sourceRange.start());
            if (sliceValue == kInvalidPlanIndex)
            {
                return;
            }
            if (target.isXmr && !target.slices.empty())
            {
                reportUnsupported(expr, "Unsupported XMR slice write");
                return;
            }
            WriteIntent intent;
            intent.target = target.target;
            intent.slices = std::move(target.slices);
            intent.value = sliceValue;
            intent.guard = guard;
            intent.domain = domain;
            intent.isNonBlocking = expr.isNonBlocking();
            intent.isXmr = target.isXmr;
            intent.xmrPath = std::move(target.xmrPath);
            intent.location = expr.sourceRange.start();
            recordWriteIntent(std::move(intent));
        };

        if (composite.reverseOrder)
        {
            uint64_t offset = 0;
            for (auto& target : targets)
            {
                const uint64_t low = offset;
                const uint64_t high = offset + target.width - 1;
                offset += target.width;
                emitSliceWrite(target, high, low);
            }
            return;
        }

        uint64_t remaining = totalWidth;
        for (auto& target : targets)
        {
            if (target.width > remaining)
            {
                reportUnsupported(expr, "LHS width exceeds RHS span");
                return;
            }
            remaining -= target.width;
            const uint64_t low = remaining;
            const uint64_t high = remaining + target.width - 1;
            emitSliceWrite(target, high, low);
        }
    }

    class AssignmentExprVisitor
        : public slang::ast::ASTVisitor<AssignmentExprVisitor, true, true> {
    public:
        explicit AssignmentExprVisitor(StmtLowererState& state) : state_(state) {}

        void handle(const slang::ast::AssignmentExpression& expr)
        {
            if (state_.suppressAssignmentWrites)
            {
                expr.left().visit(*this);
                expr.right().visit(*this);
                return;
            }
            state_.handleAssignment(expr);
        }

    private:
        StmtLowererState& state_;
    };

    EventContext buildEventContext(const slang::ast::ProceduralBlockSymbol& block)
    {
        EventContext context;
        context.procKind = mapProcKind(block.procedureKind);
        const slang::ast::TimingControl* timing = findTimingControl(block.getBody());
        if (!timing)
        {
            return context;
        }
        context.hasTiming = true;
        std::vector<EventEdge> edges;
        std::vector<ExprNodeId> operands;
        if (collectEdgeSensitiveEvents(*timing, edges, operands))
        {
            context.edgeSensitive = true;
            context.edges = std::move(edges);
            context.operands = std::move(operands);
        }
        return context;
    }

private:
    bool handleExpressionStatement(const slang::ast::Expression& expr)
    {
        const auto* call = expr.as_if<slang::ast::CallExpression>();
        if (!call)
        {
            return false;
        }
        if (call->isSystemCall())
        {
            if (handleSystemTaskCall(*call))
            {
                return true;
            }
            reportUnsupported(expr, "Unsupported system task call");
            return true;
        }

        if (auto* subroutine = std::get_if<const slang::ast::SubroutineSymbol*>(&call->subroutine))
        {
            if (*subroutine && (*subroutine)->flags.has(slang::ast::MethodFlags::DPIImport))
            {
                return handleDpiCall(*call, **subroutine);
            }
        }

        reportUnsupported(expr, "Unsupported subroutine call");
        return true;
    }

    bool handleSystemTaskCall(const slang::ast::CallExpression& call)
    {
        const std::string name = normalizeSystemTaskName(call.getSubroutineName());
        if (name == "display" || name == "write" || name == "strobe")
        {
            return emitDisplayCall(call, name);
        }
        if (name == "fwrite" || name == "fdisplay")
        {
            return emitFwriteCall(call, name);
        }
        if (name == "fclose")
        {
            return emitFcloseCall(call);
        }
        if (name == "finish" || name == "stop")
        {
            return emitFinishCall(call, name);
        }
        if (name == "info" || name == "warning" || name == "error" || name == "fatal")
        {
            return emitMessageCall(call, name);
        }
        if (name == "dumpfile")
        {
            return emitDumpfileCall(call);
        }
        if (name == "dumpvars")
        {
            return emitDumpvarsCall(call);
        }
        if (name == "fflush")
        {
            return emitFflushCall(call);
        }
        if (name == "readmemh" || name == "readmemb")
        {
            return emitReadmemCall(call, name);
        }
        if (name == "sdf_annotate" || name == "monitor" || name == "monitoron" ||
            name == "monitoroff")
        {
            if (diagnostics)
            {
                diagnostics->warn(call.sourceRange.start(),
                                  "Ignoring unsupported system task call");
            }
            return true;
        }
        return false;
    }

    bool emitDisplayCall(const slang::ast::CallExpression& call, std::string_view displayKind)
    {
        if (!ensureTaskContext(call.sourceRange.start(), "display"))
        {
            return true;
        }
        SystemTaskStmt task;
        task.name = std::string(displayKind);

        const auto args = call.arguments();
        for (const auto* arg : args)
        {
            if (!arg)
            {
                reportUnsupported(call, "Display argument is missing");
                return true;
            }
            ExprNodeId argId = lowerExpression(*arg);
            if (argId == kInvalidPlanIndex)
            {
                reportUnsupported(call, "Failed to lower display argument");
                return true;
            }
            task.args.push_back(argId);
        }

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitFwriteCall(const slang::ast::CallExpression& call, std::string_view taskName)
    {
        if (!ensureTaskContext(call.sourceRange.start(), "fwrite"))
        {
            return true;
        }
        SystemTaskStmt task;
        task.name = std::string(taskName);

        const auto args = call.arguments();
        if (args.empty())
        {
            reportUnsupported(call, "Fwrite requires a file handle expression");
            return true;
        }
        for (const auto* arg : args)
        {
            if (!arg)
            {
                reportUnsupported(call, "Fwrite argument is missing");
                return true;
            }
            ExprNodeId argId = lowerExpression(*arg);
            if (argId == kInvalidPlanIndex)
            {
                reportUnsupported(call, "Failed to lower fwrite argument");
                return true;
            }
            task.args.push_back(argId);
        }

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitMessageCall(const slang::ast::CallExpression& call, std::string_view messageKind)
    {
        if (!ensureTaskContext(call.sourceRange.start(), messageKind))
        {
            return true;
        }
        SystemTaskStmt task;
        task.name = std::string(messageKind);

        const auto args = call.arguments();
        for (const auto* arg : args)
        {
            if (!arg)
            {
                reportUnsupported(call, "Message argument is missing");
                return true;
            }
            ExprNodeId argId = lowerExpression(*arg);
            if (argId == kInvalidPlanIndex)
            {
                reportUnsupported(call, "Failed to lower message argument");
                return true;
            }
            task.args.push_back(argId);
        }

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitFinishCall(const slang::ast::CallExpression& call, std::string_view taskName)
    {
        if (!ensureTaskContext(call.sourceRange.start(), "finish"))
        {
            return true;
        }
        SystemTaskStmt task;
        task.name = std::string(taskName);
        const auto args = call.arguments();
        for (const auto* arg : args)
        {
            if (!arg)
            {
                reportUnsupported(call, "Finish argument is missing");
                return true;
            }
            ExprNodeId argId = lowerExpression(*arg);
            if (argId == kInvalidPlanIndex)
            {
                reportUnsupported(call, "Failed to lower finish argument");
                return true;
            }
            task.args.push_back(argId);
        }

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitDumpfileCall(const slang::ast::CallExpression& call)
    {
        if (!ensureTaskContext(call.sourceRange.start(), "dumpfile"))
        {
            return true;
        }
        const auto args = call.arguments();
        if (args.empty() || !args.front())
        {
            reportUnsupported(call, "$dumpfile requires a file argument");
            return true;
        }
        ExprNodeId argId = lowerExpression(*args.front());
        if (argId == kInvalidPlanIndex)
        {
            reportUnsupported(call, "Failed to lower $dumpfile argument");
            return true;
        }
        if (args.size() > 1 && diagnostics)
        {
            diagnostics->warn(call.sourceRange.start(),
                              "Ignoring extra arguments to $dumpfile");
        }
        SystemTaskStmt task;
        task.name = "dumpfile";
        task.args.push_back(argId);

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitDumpvarsCall(const slang::ast::CallExpression& call)
    {
        if (!ensureTaskContext(call.sourceRange.start(), "dumpvars"))
        {
            return true;
        }
        const auto args = call.arguments();
        auto isZeroLiteral = [&](const slang::ast::Expression& expr) -> bool {
            if (const auto* constant = expr.getConstant())
            {
                if (constant->isInteger())
                {
                    const slang::SVInt& literal = constant->integer();
                    if (!literal.hasUnknown())
                    {
                        if (auto value = literal.as<int64_t>())
                        {
                            return *value == 0;
                        }
                    }
                }
            }
            return false;
        };
        if (!args.empty())
        {
            if (args.size() == 1 && args.front() && isZeroLiteral(*args.front()))
            {
                // Accept full dump via $dumpvars(0)
            }
            else
            {
                if (diagnostics)
                {
                    diagnostics->warn(call.sourceRange.start(),
                                      "Ignoring $dumpvars with scoped arguments; only full dump is supported");
                }
                return true;
            }
        }

        SystemTaskStmt task;
        task.name = "dumpvars";

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitFflushCall(const slang::ast::CallExpression& call)
    {
        if (!ensureTaskContext(call.sourceRange.start(), "fflush"))
        {
            return true;
        }
        const auto args = call.arguments();
        SystemTaskStmt task;
        task.name = "fflush";
        if (!args.empty() && args.front())
        {
            ExprNodeId handle = lowerExpression(*args.front());
            if (handle == kInvalidPlanIndex)
            {
                reportUnsupported(call, "Failed to lower fflush file handle");
                return true;
            }
            task.args.push_back(handle);
            if (args.size() > 1 && diagnostics)
            {
                diagnostics->warn(call.sourceRange.start(),
                                  "Ignoring extra arguments to $fflush");
            }
        }

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitFcloseCall(const slang::ast::CallExpression& call)
    {
        if (!ensureTaskContext(call.sourceRange.start(), "fclose"))
        {
            return true;
        }
        const auto args = call.arguments();
        if (args.empty() || !args.front())
        {
            reportUnsupported(call, "$fclose requires a file handle expression");
            return true;
        }
        ExprNodeId handle = lowerExpression(*args.front());
        if (handle == kInvalidPlanIndex)
        {
            reportUnsupported(call, "Failed to lower fclose file handle");
            return true;
        }
        if (args.size() > 1 && diagnostics)
        {
            diagnostics->warn(call.sourceRange.start(),
                              "Ignoring extra arguments to $fclose");
        }

        SystemTaskStmt task;
        task.name = "fclose";
        task.args.push_back(handle);

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::SystemTask;
        stmt.op = wolvrix::lib::grh::OperationKind::kSystemTask;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.systemTask = std::move(task);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    bool emitReadmemCall(const slang::ast::CallExpression& call, std::string_view taskName)
    {
        if (eventContext.procKind != ProcKind::Initial &&
            eventContext.procKind != ProcKind::Final)
        {
            if (diagnostics)
            {
                diagnostics->warn(call.sourceRange.start(),
                                  "Treating $readmemh/$readmemb outside initial/final as memory init");
            }
        }

        const auto args = call.arguments();
        if (args.size() < 2 || !args[0] || !args[1])
        {
            reportUnsupported(call, "$readmemh/$readmemb requires file and memory arguments");
            return true;
        }

        auto fileLiteral = extractStringLiteral(*args[0]);
        if (!fileLiteral)
        {
            reportUnsupported(call, "$readmemh/$readmemb file must be a string literal");
            return true;
        }

        PlanSymbolId memSymbol = resolveSimpleSymbol(*args[1]);
        if (!memSymbol.valid())
        {
            reportUnsupported(call, "$readmemh/$readmemb requires a simple memory symbol");
            return true;
        }

        const SignalInfo* signal = findSignalBySymbol(plan, memSymbol);
        if (!signal || (signal->memoryRows <= 0 && signal->kind != SignalKind::Memory))
        {
            if (diagnostics)
            {
                diagnostics->warn(call.sourceRange.start(),
                                  "Ignoring $readmemh/$readmemb for non-memory symbol");
            }
            return true;
        }

        MemoryInit init;
        init.memory = memSymbol;
        init.kind = std::string(taskName);
        init.file = std::move(*fileLiteral);
        init.location = call.sourceRange.start();

        if (args.size() > 2 && args[2])
        {
            ExprNodeId startExpr = lowerExpression(*args[2]);
            if (startExpr == kInvalidPlanIndex)
            {
                reportUnsupported(call, "$readmemh/$readmemb start expression is unsupported");
                return true;
            }
            if (auto startConst = evalConstInt(plan, lowering, startExpr))
            {
                init.start = *startConst;
            }
            else if (diagnostics)
            {
                diagnostics->warn(call.sourceRange.start(),
                                  "Ignoring $readmemh/$readmemb start expression; must be constant");
            }
        }

        if (args.size() > 3 && args[3])
        {
            if (init.start < 0)
            {
                if (diagnostics)
                {
                    diagnostics->warn(call.sourceRange.start(),
                                      "Ignoring $readmemh/$readmemb finish without constant start");
                }
            }
            else
            {
                ExprNodeId finishExpr = lowerExpression(*args[3]);
                if (finishExpr == kInvalidPlanIndex)
                {
                    reportUnsupported(call, "$readmemh/$readmemb finish expression is unsupported");
                    return true;
                }
                if (auto finishConst = evalConstInt(plan, lowering, finishExpr))
                {
                    init.len = *finishConst - init.start + 1;
                }
                else if (diagnostics)
                {
                    diagnostics->warn(call.sourceRange.start(),
                                      "Ignoring $readmemh/$readmemb finish expression; must be constant");
                }
            }
        }

        if (args.size() > 4 && diagnostics)
        {
            diagnostics->warn(call.sourceRange.start(),
                              "Ignoring extra arguments to $readmemh/$readmemb");
        }

        for (const auto& existing : lowering.memoryInits)
        {
            if (existing.memory.index != init.memory.index ||
                existing.kind != init.kind ||
                existing.file != init.file ||
                existing.initValue != init.initValue ||
                existing.start != init.start ||
                existing.len != init.len ||
                existing.location != init.location)
            {
                continue;
            }
            return true;
        }

        lowering.memoryInits.push_back(std::move(init));
        return true;
    }

    bool handleDpiCall(const slang::ast::CallExpression& call,
                       const slang::ast::SubroutineSymbol& subroutine)
    {
        if (!ensureEdgeSensitive(call.sourceRange.start(), "dpi"))
        {
            return true;
        }
        if (subroutine.subroutineKind != slang::ast::SubroutineKind::Function)
        {
            reportUnsupported(call, "DPI call supports only functions");
            return true;
        }

        const auto args = call.arguments();
        const auto formals = subroutine.getArguments();
        if (args.size() != formals.size())
        {
            reportUnsupported(call, "DPI call argument count mismatch");
            return true;
        }

        DpiCallStmt dpi;
        dpi.targetImportSymbol = std::string(subroutine.name);

        for (std::size_t i = 0; i < formals.size(); ++i)
        {
            const auto* formal = formals[i];
            const auto* actual = args[i];
            if (!formal || !actual)
            {
                reportUnsupported(call, "DPI call missing argument");
                return true;
            }

            switch (formal->direction)
            {
            case slang::ast::ArgumentDirection::In: {
                const slang::ast::Expression& actualExpr = unwrapDpiArgument(*actual, false);
                ExprNodeId argId = lowerExpression(actualExpr);
                if (argId == kInvalidPlanIndex)
                {
                    reportUnsupported(call, "Failed to lower DPI input argument");
                    return true;
                }
                dpi.inArgNames.emplace_back(formal->name);
                dpi.inArgs.push_back(argId);
                break;
            }
            case slang::ast::ArgumentDirection::Out: {
                const slang::ast::Expression& actualExpr = unwrapDpiArgument(*actual, true);
                PlanSymbolId targetSymbol = resolveSimpleSymbol(actualExpr);
                if (!targetSymbol.valid())
                {
                    std::string message("Unsupported DPI output argument kind: ");
                    message.append(std::string(slang::ast::toString(actualExpr.kind)));
                    reportUnsupported(call, std::move(message));
                    return true;
                }
                PlanSymbolId resultSymbol = makeDpiResultSymbol();
                dpi.outArgNames.emplace_back(formal->name);
                dpi.results.push_back(resultSymbol);
                WriteIntent intent;
                intent.target = targetSymbol;
                intent.value = makeSymbolExpr(resultSymbol, call.sourceRange.start());
                intent.guard = currentGuard(call.sourceRange.start());
                intent.domain = domain;
                intent.isNonBlocking = false;
                intent.location = call.sourceRange.start();
                recordWriteIntent(std::move(intent));
                break;
            }
            default:
                reportUnsupported(call, "DPI call supports only input/output arguments");
                return true;
            }
        }

        const bool hasReturn = subroutine.getReturnType().isVoid() == false;
        dpi.hasReturn = hasReturn;
        if (hasReturn)
        {
            PlanSymbolId retSymbol = makeDpiResultSymbol();
            dpi.results.insert(dpi.results.begin(), retSymbol);
        }

        if (!recordDpiImport(subroutine, call.sourceRange.start()))
        {
            return true;
        }

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::DpiCall;
        stmt.op = wolvrix::lib::grh::OperationKind::kDpicCall;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.dpiCall = std::move(dpi);
        lowering.loweredStmts.push_back(std::move(stmt));
        return true;
    }

    ExprNodeId handleDpiCallExpr(const slang::ast::CallExpression& call,
                                 const slang::ast::SubroutineSymbol& subroutine,
                                 const slang::ast::Expression* cacheKey)
    {
        if (!ensureEdgeSensitive(call.sourceRange.start(), "dpi"))
        {
            return kInvalidPlanIndex;
        }
        if (subroutine.subroutineKind != slang::ast::SubroutineKind::Function)
        {
            reportUnsupported(call, "DPI call supports only functions");
            return kInvalidPlanIndex;
        }
        if (subroutine.getReturnType().isVoid())
        {
            reportUnsupported(call, "DPI call without return cannot be used in expressions");
            return kInvalidPlanIndex;
        }

        const auto args = call.arguments();
        const auto formals = subroutine.getArguments();
        if (args.size() != formals.size())
        {
            reportUnsupported(call, "DPI call argument count mismatch");
            return kInvalidPlanIndex;
        }

        DpiCallStmt dpi;
        dpi.targetImportSymbol = std::string(subroutine.name);

        for (std::size_t i = 0; i < formals.size(); ++i)
        {
            const auto* formal = formals[i];
            const auto* actual = args[i];
            if (!formal || !actual)
            {
                reportUnsupported(call, "DPI call missing argument");
                return kInvalidPlanIndex;
            }

            switch (formal->direction)
            {
            case slang::ast::ArgumentDirection::In: {
                const slang::ast::Expression& actualExpr = unwrapDpiArgument(*actual, false);
                ExprNodeId argId = lowerExpression(actualExpr);
                if (argId == kInvalidPlanIndex)
                {
                    reportUnsupported(call, "Failed to lower DPI input argument");
                    return kInvalidPlanIndex;
                }
                dpi.inArgNames.emplace_back(formal->name);
                dpi.inArgs.push_back(argId);
                break;
            }
            case slang::ast::ArgumentDirection::Out: {
                const slang::ast::Expression& actualExpr = unwrapDpiArgument(*actual, true);
                PlanSymbolId targetSymbol = resolveSimpleSymbol(actualExpr);
                if (!targetSymbol.valid())
                {
                    std::string message("Unsupported DPI output argument kind: ");
                    message.append(std::string(slang::ast::toString(actualExpr.kind)));
                    reportUnsupported(call, std::move(message));
                    return kInvalidPlanIndex;
                }
                PlanSymbolId resultSymbol = makeDpiResultSymbol();
                dpi.outArgNames.emplace_back(formal->name);
                dpi.results.push_back(resultSymbol);
                WriteIntent intent;
                intent.target = targetSymbol;
                intent.value = makeSymbolExpr(resultSymbol, call.sourceRange.start());
                intent.guard = currentGuard(call.sourceRange.start());
                intent.domain = domain;
                intent.isNonBlocking = false;
                intent.location = call.sourceRange.start();
                recordWriteIntent(std::move(intent));
                break;
            }
            default:
                reportUnsupported(call, "DPI call supports only input/output arguments");
                return kInvalidPlanIndex;
            }
        }

        dpi.hasReturn = true;
        PlanSymbolId retSymbol = makeDpiResultSymbol();
        dpi.results.insert(dpi.results.begin(), retSymbol);

        if (!recordDpiImport(subroutine, call.sourceRange.start()))
        {
            return kInvalidPlanIndex;
        }

        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::DpiCall;
        stmt.op = wolvrix::lib::grh::OperationKind::kDpicCall;
        stmt.updateCond = ensureGuardExpr(currentGuard(call.sourceRange.start()),
                                          call.sourceRange.start());
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        stmt.location = call.sourceRange.start();
        stmt.dpiCall = std::move(dpi);
        lowering.loweredStmts.push_back(std::move(stmt));

        ExprNode node;
        node.kind = ExprNodeKind::Symbol;
        node.symbol = retSymbol;
        node.location = call.sourceRange.start();
        node.isSigned = subroutine.getReturnType().isSigned();
        uint64_t width = computeFixedWidth(subroutine.getReturnType(), subroutine, diagnostics);
        if (width > 0)
        {
            const uint64_t maxValue =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            node.widthHint = width > maxValue
                                 ? std::numeric_limits<int32_t>::max()
                                 : static_cast<int32_t>(width);
        }
        return addNode(cacheKey, std::move(node));
    }

    bool recordDpiImport(const slang::ast::SubroutineSymbol& subroutine,
                         slang::SourceLocation location)
    {
        auto info = buildDpiImportInfo(subroutine, location);
        if (!info)
        {
            return false;
        }
        for (const auto& existing : lowering.dpiImports)
        {
            if (existing.symbol != info->symbol)
            {
                continue;
            }
            if (!dpiImportSignatureMatches(existing, *info))
            {
                if (diagnostics)
                {
                    diagnostics->error(
                        location,
                        std::string("Conflicting DPI import signature for ") + existing.symbol);
                }
                return false;
            }
            return true;
        }
        lowering.dpiImports.push_back(std::move(*info));
        return true;
    }

    std::optional<DpiImportInfo> buildDpiImportInfo(
        const slang::ast::SubroutineSymbol& subroutine,
        slang::SourceLocation location)
    {
        DpiImportInfo info;
        info.symbol = std::string(subroutine.name);
        auto classifyDpiType = [](const slang::ast::Type& type) -> std::string {
            const slang::ast::Type& canonical = type.getCanonicalType();
            if (canonical.isString())
            {
                return "string";
            }
            if (canonical.isFloating())
            {
                const auto& floatType = canonical.as<slang::ast::FloatingType>();
                switch (floatType.floatKind)
                {
                case slang::ast::FloatingType::Real:
                    return "real";
                case slang::ast::FloatingType::ShortReal:
                    return "shortreal";
                case slang::ast::FloatingType::RealTime:
                    return "realtime";
                }
            }
            if (canonical.isCHandle())
            {
                return "chandle";
            }

            const slang::ast::Type* base = &canonical;
            while (base->kind == slang::ast::SymbolKind::PackedArrayType)
            {
                base = &base->as<slang::ast::PackedArrayType>().elementType.getCanonicalType();
            }
            if (base->kind == slang::ast::SymbolKind::ScalarType)
            {
                const auto& scalar = base->as<slang::ast::ScalarType>();
                return scalar.scalarKind == slang::ast::ScalarType::Bit ? "bit" : "logic";
            }
            if (base->kind == slang::ast::SymbolKind::PredefinedIntegerType)
            {
                const auto& intType = base->as<slang::ast::PredefinedIntegerType>();
                switch (intType.integerKind)
                {
                case slang::ast::PredefinedIntegerType::ShortInt:
                    return "shortint";
                case slang::ast::PredefinedIntegerType::Int:
                    return "int";
                case slang::ast::PredefinedIntegerType::LongInt:
                    return "longint";
                case slang::ast::PredefinedIntegerType::Byte:
                    return "byte";
                case slang::ast::PredefinedIntegerType::Integer:
                    return "integer";
                case slang::ast::PredefinedIntegerType::Time:
                    return "time";
                }
            }
            return "logic";
        };
        const auto formals = subroutine.getArguments();
        info.argsDirection.reserve(formals.size());
        info.argsWidth.reserve(formals.size());
        info.argsName.reserve(formals.size());
        info.argsSigned.reserve(formals.size());
        info.argsType.reserve(formals.size());

        for (const auto* formal : formals)
        {
            if (!formal)
            {
                if (diagnostics)
                {
                    diagnostics->error(location, "DPI import missing formal argument");
                }
                return std::nullopt;
            }
            std::string direction;
            switch (formal->direction)
            {
            case slang::ast::ArgumentDirection::In:
                direction = "input";
                break;
            case slang::ast::ArgumentDirection::Out:
                direction = "output";
                break;
            default:
                if (diagnostics)
                {
                    diagnostics->error(*formal,
                                       "Unsupported DPI argument direction");
                }
                return std::nullopt;
            }
            const slang::ast::Type& formalType = formal->getType();
            const std::string typeName = classifyDpiType(formalType);
            int32_t width = 1;
            bool isSigned = false;
            if (formalType.isIntegral())
            {
                const uint64_t widthRaw =
                    computeFixedWidth(formalType, *formal, diagnostics);
                width = clampWidth(widthRaw, *formal, diagnostics, "DPI argument");
                isSigned = formalType.isSigned();
            }
            info.argsDirection.push_back(std::move(direction));
            info.argsWidth.push_back(static_cast<int64_t>(width));
            info.argsName.push_back(std::string(formal->name));
            info.argsSigned.push_back(isSigned);
            info.argsType.push_back(typeName);
        }

        const slang::ast::Type& returnType = subroutine.getReturnType();
        if (!returnType.isVoid())
        {
            const std::string typeName = classifyDpiType(returnType);
            int32_t width = 1;
            bool isSigned = false;
            if (returnType.isIntegral())
            {
                const uint64_t widthRaw =
                    computeFixedWidth(returnType, subroutine, diagnostics);
                width = clampWidth(widthRaw, subroutine, diagnostics, "DPI return");
                isSigned = returnType.isSigned();
            }
            info.hasReturn = true;
            info.returnWidth = static_cast<int64_t>(width);
            info.returnSigned = isSigned;
            info.returnType = typeName;
        }
        return info;
    }

    static bool dpiImportSignatureMatches(const DpiImportInfo& lhs, const DpiImportInfo& rhs)
    {
        return lhs.symbol == rhs.symbol &&
               lhs.argsDirection == rhs.argsDirection &&
               lhs.argsWidth == rhs.argsWidth &&
               lhs.argsName == rhs.argsName &&
               lhs.argsSigned == rhs.argsSigned &&
               lhs.argsType == rhs.argsType &&
               lhs.hasReturn == rhs.hasReturn &&
               lhs.returnWidth == rhs.returnWidth &&
               lhs.returnSigned == rhs.returnSigned &&
               lhs.returnType == rhs.returnType;
    }

    void recordWriteIntent(WriteIntent intent)
    {
        if (currentCaseCoverage())
        {
            intent.coversAllTwoState = true;
        }
        LoweredStmt stmt;
        stmt.kind = LoweredStmtKind::Write;
        stmt.op = wolvrix::lib::grh::OperationKind::kAssign;
        stmt.location = intent.location;
        stmt.procKind = eventContext.procKind;
        stmt.hasTiming = eventContext.hasTiming;
        stmt.write = intent;
        stmt.updateCond = ensureGuardExpr(stmt.write.guard, stmt.location);
        stmt.eventEdges = eventContext.edges;
        stmt.eventOperands = eventContext.operands;
        lowering.loweredStmts.push_back(std::move(stmt));
        if (!intent.isXmr)
        {
            lowering.writes.push_back(std::move(intent));
        }
    }

    const slang::ast::Expression& unwrapDpiArgument(const slang::ast::Expression& expr,
                                                    bool output) const
    {
        if (const auto* assignment = expr.as_if<slang::ast::AssignmentExpression>())
        {
            return output ? assignment->left() : assignment->right();
        }
        return expr;
    }

    PlanSymbolId resolveSimpleSymbol(const slang::ast::Expression& expr)
    {
        if (const auto* assignment = expr.as_if<slang::ast::AssignmentExpression>())
        {
            if (assignment->isLValueArg())
            {
                return resolveSimpleSymbol(assignment->left());
            }
            return resolveSimpleSymbol(assignment->right());
        }
        if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
        {
            if (auto local = lookupProceduralLocal(named->symbol))
            {
                return *local;
            }
            return lookupPlanSignal(plan, named->symbol);
        }
        if (const auto* hier = expr.as_if<slang::ast::HierarchicalValueExpression>())
        {
            (void)hier;
            return {};
        }
        if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
        {
            return resolveSimpleSymbol(conversion->operand());
        }
        return {};
    }

    std::optional<std::string> extractStringLiteral(const slang::ast::Expression& expr)
    {
        if (const auto* literal = expr.as_if<slang::ast::StringLiteral>())
        {
            return std::string(literal->getValue());
        }
        if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
        {
            if (conversion->isImplicit())
            {
                return extractStringLiteral(conversion->operand());
            }
        }
        return std::nullopt;
    }

    bool isIntegerLiteralExpr(const slang::ast::Expression& expr)
    {
        if (expr.as_if<slang::ast::IntegerLiteral>() ||
            expr.as_if<slang::ast::UnbasedUnsizedIntegerLiteral>())
        {
            return true;
        }
        if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
        {
            if (conversion->isImplicit())
            {
                return isIntegerLiteralExpr(conversion->operand());
            }
        }
        return false;
    }

    bool ensureEdgeSensitive(slang::SourceLocation location, std::string_view label)
    {
        if (eventContext.edgeSensitive && !eventContext.operands.empty())
        {
            return true;
        }
        if (diagnostics)
        {
            std::string message("Ignoring ");
            message.append(label);
            message.append(" call without edge-sensitive timing control");
            diagnostics->warn(location, std::move(message));
        }
        return false;
    }

    bool ensureTaskContext(slang::SourceLocation location, std::string_view label)
    {
        if (eventContext.procKind == ProcKind::Initial ||
            eventContext.procKind == ProcKind::Final)
        {
            return true;
        }
        if (eventContext.edgeSensitive && !eventContext.operands.empty())
        {
            return true;
        }
        if (eventContext.procKind == ProcKind::AlwaysComb ||
            eventContext.procKind == ProcKind::AlwaysLatch ||
            eventContext.procKind == ProcKind::Always)
        {
            return true;
        }
        if (diagnostics)
        {
            std::string message("Ignoring ");
            message.append(label);
            message.append(" call without supported timing context");
            diagnostics->warn(location, std::move(message));
        }
        return false;
    }

    bool collectEdgeSensitiveEvents(const slang::ast::TimingControl& timing,
                                    std::vector<EventEdge>& edges,
                                    std::vector<ExprNodeId>& operands)
    {
        std::vector<EventEdge> tempEdges;
        std::vector<ExprNodeId> tempOperands;
        if (!appendEdgeSensitiveEvents(timing, tempEdges, tempOperands))
        {
            return false;
        }
        if (tempEdges.empty())
        {
            return false;
        }
        edges = std::move(tempEdges);
        operands = std::move(tempOperands);
        return true;
    }

    bool appendEdgeSensitiveEvents(const slang::ast::TimingControl& timing,
                                   std::vector<EventEdge>& edges,
                                   std::vector<ExprNodeId>& operands)
    {
        using slang::ast::EdgeKind;
        using slang::ast::TimingControlKind;
        switch (timing.kind)
        {
        case TimingControlKind::SignalEvent: {
            const auto& signal = timing.as<slang::ast::SignalEventControl>();
            if (signal.edge == EdgeKind::None || signal.edge == EdgeKind::BothEdges)
            {
                return false;
            }
            if (signal.iffCondition)
            {
                if (diagnostics)
                {
                    diagnostics->warn(signal.sourceRange.start(),
                                      "Ignoring event control with iff condition");
                }
                return false;
            }
            ExprNodeId operand = lowerExpression(signal.expr);
            if (operand == kInvalidPlanIndex)
            {
                return false;
            }
            EventEdge edge = signal.edge == EdgeKind::PosEdge ? EventEdge::Posedge
                                                              : EventEdge::Negedge;
            edges.push_back(edge);
            operands.push_back(operand);
            return true;
        }
        case TimingControlKind::EventList: {
            const auto& list = timing.as<slang::ast::EventListControl>();
            for (const slang::ast::TimingControl* child : list.events)
            {
                if (!child)
                {
                    continue;
                }
                if (!appendEdgeSensitiveEvents(*child, edges, operands))
                {
                    return false;
                }
            }
            return true;
        }
        case TimingControlKind::RepeatedEvent:
        case TimingControlKind::ImplicitEvent:
        case TimingControlKind::Delay:
        case TimingControlKind::Delay3:
        case TimingControlKind::OneStepDelay:
        case TimingControlKind::CycleDelay:
        case TimingControlKind::BlockEventList:
        case TimingControlKind::Invalid:
        default:
            return false;
        }
    }

    ExprNodeId currentPathGuard() const
    {
        if (guardStack.empty())
        {
            return kInvalidPlanIndex;
        }
        return guardStack.back();
    }

    ExprNodeId currentFlowGuard() const
    {
        if (flowStack.empty())
        {
            return kInvalidPlanIndex;
        }
        return flowStack.back();
    }

    ExprNodeId currentGuard(slang::SourceLocation location)
    {
        return combineGuard(currentPathGuard(), currentFlowGuard(), location);
    }

    void pushGuard(ExprNodeId guard)
    {
        guardStack.push_back(guard);
    }

    void popGuard()
    {
        if (!guardStack.empty())
        {
            guardStack.pop_back();
        }
    }

    void pushFlowGuard(ExprNodeId guard)
    {
        flowStack.push_back(guard);
    }

    void popFlowGuard()
    {
        if (!flowStack.empty())
        {
            flowStack.pop_back();
        }
    }

    void updateFlowGuard(ExprNodeId guard)
    {
        if (flowStack.empty())
        {
            return;
        }
        flowStack.back() = guard;
    }

    bool inDynamicLoop() const
    {
        return !loopFlowStack.empty();
    }

    ExprNodeId currentLoopAlive() const
    {
        if (loopFlowStack.empty())
        {
            return kInvalidPlanIndex;
        }
        return loopFlowStack.back().loopAlive;
    }

    void updateLoopAlive(ExprNodeId guard)
    {
        if (loopFlowStack.empty())
        {
            return;
        }
        loopFlowStack.back().loopAlive = guard;
    }

    void pushLoopContext(slang::SourceLocation location)
    {
        LoopFlowContext context;
        context.loopAlive = addConstantLiteral("1'b1", location);
        loopFlowStack.push_back(std::move(context));
    }

    void popLoopContext()
    {
        if (!loopFlowStack.empty())
        {
            loopFlowStack.pop_back();
        }
    }

    void pushLoopLocals(std::vector<const slang::ast::ValueSymbol*> vars)
    {
        loopLocalStack.emplace_back();
        loopVarStack.emplace_back(std::move(vars));
    }

    void popLoopLocals()
    {
        if (!loopLocalStack.empty())
        {
            loopLocalStack.pop_back();
        }
        if (!loopVarStack.empty())
        {
            loopVarStack.pop_back();
        }
    }

    void syncLoopLocals(slang::ast::EvalContext& ctx)
    {
        if (loopLocalStack.empty() || loopVarStack.empty())
        {
            return;
        }
        auto& locals = loopLocalStack.back();
        locals.clear();
        for (const slang::ast::ValueSymbol* symbol : loopVarStack.back())
        {
            if (!symbol)
            {
                continue;
            }
            if (const slang::ConstantValue* value = ctx.findLocal(symbol))
            {
                if (value->isInteger() && !value->hasUnknown())
                {
                    if (auto raw = value->integer().as<int64_t>())
                    {
                        locals.emplace(symbol, *raw);
                    }
                }
            }
        }
    }

    bool isLoopLocalSymbol(const slang::ast::ValueSymbol& symbol) const
    {
        for (auto it = loopVarStack.rbegin(); it != loopVarStack.rend(); ++it)
        {
            for (const slang::ast::ValueSymbol* entry : *it)
            {
                if (entry == &symbol)
                {
                    return true;
                }
                if (!entry)
                {
                    continue;
                }
                if (!symbol.name.empty() && entry->name == symbol.name)
                {
                    return true;
                }
            }
        }
        return false;
    }

    std::optional<int64_t> lookupLoopLocal(const slang::ast::Symbol& symbol) const
    {
        const auto* valueSymbol = symbol.as_if<slang::ast::ValueSymbol>();
        if (!valueSymbol)
        {
            return std::nullopt;
        }
        auto localsIt = loopLocalStack.rbegin();
        auto varsIt = loopVarStack.rbegin();
        for (; localsIt != loopLocalStack.rend() && varsIt != loopVarStack.rend();
             ++localsIt, ++varsIt)
        {
            if (auto found = localsIt->find(valueSymbol); found != localsIt->end())
            {
                return found->second;
            }
            if (valueSymbol->name.empty())
            {
                continue;
            }
            for (const slang::ast::ValueSymbol* entry : *varsIt)
            {
                if (!entry || entry->name != valueSymbol->name)
                {
                    continue;
                }
                if (auto found = localsIt->find(entry); found != localsIt->end())
                {
                    return found->second;
                }
            }
        }
        return std::nullopt;
    }

public:
    void pushProceduralValues()
    {
        proceduralValueStack.emplace_back();
        pushProceduralLocalScope();
    }

    void popProceduralValues()
    {
        if (!proceduralLocalStack.empty())
        {
            popProceduralLocalScope();
        }
        if (!proceduralValueStack.empty())
        {
            proceduralValueStack.pop_back();
        }
    }

    bool hasProceduralValues() const
    {
        return !proceduralValueStack.empty();
    }

    void pushProceduralLocalScope()
    {
        proceduralLocalStack.emplace_back();
    }

    void popProceduralLocalScope()
    {
        if (proceduralLocalStack.empty())
        {
            return;
        }
        auto locals = std::move(proceduralLocalStack.back());
        proceduralLocalStack.pop_back();
        if (!proceduralValueStack.empty())
        {
            auto& values = proceduralValueStack.back();
            for (const auto& entry : locals)
            {
                values.erase(entry.second.index);
                proceduralLocalTypes.erase(entry.second.index);
            }
        }
    }

    PlanSymbolId internProceduralLocal(const slang::ast::ValueSymbol& symbol)
    {
        if (!hasProceduralValues())
        {
            return {};
        }
        if (proceduralLocalStack.empty())
        {
            pushProceduralLocalScope();
        }
        if (auto found = lookupProceduralLocal(symbol))
        {
            return *found;
        }
        PlanSymbolId id = makeInternalPlanValueSymbol(plan);
        proceduralLocalStack.back()[&symbol] = id;
        proceduralLocalTypes[id.index] = &symbol.getType();
        return id;
    }

    std::optional<PlanSymbolId>
    lookupProceduralLocal(const slang::ast::Symbol& symbol) const
    {
        const auto* valueSymbol = symbol.as_if<slang::ast::ValueSymbol>();
        if (!valueSymbol)
        {
            return std::nullopt;
        }
        for (auto it = proceduralLocalStack.rbegin(); it != proceduralLocalStack.rend();
             ++it)
        {
            if (auto found = it->find(valueSymbol); found != it->end())
            {
                return found->second;
            }
            if (valueSymbol->name.empty())
            {
                continue;
            }
            for (const auto& entry : *it)
            {
                if (entry.first && entry.first->name == valueSymbol->name)
                {
                    return entry.second;
                }
            }
        }
        return std::nullopt;
    }

    bool isProceduralLocalSymbol(PlanSymbolId symbol) const
    {
        return symbol.valid() && proceduralLocalTypes.find(symbol.index) !=
                                     proceduralLocalTypes.end();
    }

    ExprNodeId makeProceduralLocalDefault(PlanSymbolId symbol,
                                          slang::SourceLocation location)
    {
        int32_t width = 1;
        if (symbol.valid())
        {
            if (auto found = proceduralLocalTypes.find(symbol.index);
                found != proceduralLocalTypes.end() && found->second && plan.body)
            {
                const uint64_t widthRaw =
                    computeFixedWidth(*found->second, *plan.body, diagnostics);
                if (widthRaw > 0)
                {
                    const uint64_t maxValue =
                        static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                    width = widthRaw > maxValue
                                ? std::numeric_limits<int32_t>::max()
                                : static_cast<int32_t>(widthRaw);
                }
            }
        }
        ExprNode node;
        node.kind = ExprNodeKind::Constant;
        node.literal = width > 1 ? std::to_string(width) + "'b0" : "1'b0";
        node.location = location;
        node.widthHint = width;
        return addNode(nullptr, std::move(node));
    }

    std::optional<ExprNodeId> lookupProceduralValue(const slang::ast::Symbol& symbol) const
    {
        if (proceduralValueStack.empty())
        {
            return std::nullopt;
        }
        const auto* valueSymbol = symbol.as_if<slang::ast::ValueSymbol>();
        if (!valueSymbol || valueSymbol->name.empty())
        {
            return std::nullopt;
        }
        PlanSymbolId id;
        if (auto local = lookupProceduralLocal(symbol))
        {
            id = *local;
        }
        else
        {
            id = lookupPlanSignal(plan, *valueSymbol);
        }
        if (!id.valid())
        {
            return std::nullopt;
        }
        for (auto it = proceduralValueStack.rbegin(); it != proceduralValueStack.rend(); ++it)
        {
            if (auto found = it->find(id.index); found != it->end())
            {
                return found->second;
            }
        }
        return std::nullopt;
    }

    std::optional<ExprNodeId> lookupProceduralValue(PlanSymbolId symbol) const
    {
        if (!symbol.valid() || proceduralValueStack.empty())
        {
            return std::nullopt;
        }
        for (auto it = proceduralValueStack.rbegin(); it != proceduralValueStack.rend();
             ++it)
        {
            if (auto found = it->find(symbol.index); found != it->end())
            {
                return found->second;
            }
        }
        return std::nullopt;
    }

    void updateProceduralValue(PlanSymbolId symbol, ExprNodeId value)
    {
        if (!symbol.valid() || proceduralValueStack.empty())
        {
            return;
        }
        proceduralValueStack.back()[symbol.index] = value;
    }

private:

    bool exprUsesLoopLocal(const slang::ast::Expression& expr) const
    {
        if (loopVarStack.empty())
        {
            return false;
        }
        struct LoopLocalVisitor
            : public slang::ast::ASTVisitor<LoopLocalVisitor, true, true> {
            explicit LoopLocalVisitor(const StmtLowererState& state) : state_(state) {}

            void handle(const slang::ast::NamedValueExpression& expr)
            {
                if (state_.isLoopLocalSymbol(expr.symbol))
                {
                    found_ = true;
                }
            }

            void handle(const slang::ast::HierarchicalValueExpression& expr)
            {
                if (state_.isLoopLocalSymbol(expr.symbol))
                {
                    found_ = true;
                }
            }

            const StmtLowererState& state_;
            bool found_ = false;
        };

        LoopLocalVisitor visitor(*this);
        expr.visit(visitor);
        return visitor.found_;
    }

    bool exprUsesProceduralValue(const slang::ast::Expression& expr) const
    {
        if (proceduralValueStack.empty())
        {
            return false;
        }
        struct ProcValueVisitor
            : public slang::ast::ASTVisitor<ProcValueVisitor, true, true> {
            explicit ProcValueVisitor(const StmtLowererState& state) : state_(state) {}

            void handle(const slang::ast::NamedValueExpression& expr)
            {
                if (state_.lookupProceduralValue(expr.symbol))
                {
                    found_ = true;
                }
            }

            void handle(const slang::ast::HierarchicalValueExpression& expr)
            {
                if (state_.lookupProceduralValue(expr.symbol))
                {
                    found_ = true;
                }
            }

            const StmtLowererState& state_;
            bool found_ = false;
        };

        ProcValueVisitor visitor(*this);
        expr.visit(visitor);
        return visitor.found_;
    }

    bool exprUsesUnknownSymbol(const slang::ast::Expression& expr) const
    {
        struct UnknownSymbolVisitor
            : public slang::ast::ASTVisitor<UnknownSymbolVisitor, true, true> {
            explicit UnknownSymbolVisitor(const StmtLowererState& state) : state_(state) {}

            void handle(const slang::ast::NamedValueExpression& expr)
            {
                if (expr.symbol.kind == slang::ast::SymbolKind::Parameter ||
                    expr.symbol.kind == slang::ast::SymbolKind::TypeParameter)
                {
                    return;
                }
                PlanSymbolId id = lookupPlanSignal(state_.plan, expr.symbol);
                if (!id.valid())
                {
                    found_ = true;
                }
            }

            void handle(const slang::ast::HierarchicalValueExpression& expr)
            {
                if (expr.symbol.kind == slang::ast::SymbolKind::Parameter ||
                    expr.symbol.kind == slang::ast::SymbolKind::TypeParameter)
                {
                    return;
                }
                if (!extractXmrPath(expr))
                {
                    found_ = true;
                }
            }

            const StmtLowererState& state_;
            bool found_ = false;
        };

        UnknownSymbolVisitor visitor(*this);
        expr.visit(visitor);
        return visitor.found_;
    }

    bool handleLoopBreak(const slang::ast::Statement& stmt)
    {
        if (!inDynamicLoop())
        {
            return false;
        }
        const auto location = stmt.sourceRange.start();
        ExprNodeId trigger = ensureGuardExpr(currentGuard(location), location);
        ExprNodeId notTrigger = makeLogicNot(trigger, location);
        updateFlowGuard(combineGuard(currentFlowGuard(), notTrigger, location));
        updateLoopAlive(combineGuard(currentLoopAlive(), notTrigger, location));
        return true;
    }

    bool handleLoopContinue(const slang::ast::Statement& stmt)
    {
        if (!inDynamicLoop())
        {
            return false;
        }
        const auto location = stmt.sourceRange.start();
        ExprNodeId trigger = ensureGuardExpr(currentGuard(location), location);
        ExprNodeId notTrigger = makeLogicNot(trigger, location);
        updateFlowGuard(combineGuard(currentFlowGuard(), notTrigger, location));
        return true;
    }

    ExprNodeId combineGuard(ExprNodeId base, ExprNodeId extra,
                            slang::SourceLocation location)
    {
        if (base == kInvalidPlanIndex)
        {
            return extra;
        }
        if (extra == kInvalidPlanIndex)
        {
            return base;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kLogicAnd, {base, extra}, location);
    }

    ExprNodeId ensureGuardExpr(ExprNodeId guard, slang::SourceLocation location)
    {
        if (guard != kInvalidPlanIndex)
        {
            return guard;
        }
        return addConstantLiteral("1'b1", location);
    }

    ExprNodeId makeLogicAnd(ExprNodeId lhs, ExprNodeId rhs, slang::SourceLocation location)
    {
        if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kLogicAnd, {lhs, rhs}, location);
    }

    ExprNodeId makeLogicOr(ExprNodeId lhs, ExprNodeId rhs, slang::SourceLocation location)
    {
        if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kLogicOr, {lhs, rhs}, location);
    }

    ExprNodeId makeLogicNot(ExprNodeId operand, slang::SourceLocation location)
    {
        if (operand == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kLogicNot, {operand}, location);
    }

    ExprNodeId makeMux(ExprNodeId cond, ExprNodeId lhs, ExprNodeId rhs,
                       slang::SourceLocation location)
    {
        if (cond == kInvalidPlanIndex || lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kMux, {cond, lhs, rhs}, location);
    }

    ExprNodeId makeEq(ExprNodeId lhs, ExprNodeId rhs, slang::SourceLocation location)
    {
        if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kEq, {lhs, rhs}, location);
    }

    ExprNodeId makeCaseEq(ExprNodeId lhs, ExprNodeId rhs, slang::SourceLocation location)
    {
        if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kCaseEq, {lhs, rhs}, location);
    }

    void warnIgnoredStatement(const slang::ast::Statement& stmt, std::string_view label)
    {
        if (!diagnostics)
        {
            return;
        }
        std::string message("Ignoring ");
        message.append(label);
        message.append("; lowering statement without timing semantics");
        diagnostics->warn(stmt.sourceRange.start(), std::move(message));
    }

    std::string describeTiming(const slang::ast::TimingControl& timing) const
    {
        using slang::ast::TimingControlKind;
        switch (timing.kind)
        {
        case TimingControlKind::Delay:
            return "delay";
        case TimingControlKind::Delay3:
            return "delay3";
        case TimingControlKind::OneStepDelay:
            return "one-step delay";
        case TimingControlKind::CycleDelay:
            return "cycle delay";
        case TimingControlKind::SignalEvent:
            return "event";
        case TimingControlKind::EventList:
            return "event list";
        case TimingControlKind::ImplicitEvent:
            return "implicit event";
        case TimingControlKind::RepeatedEvent:
            return "repeated event";
        case TimingControlKind::BlockEventList:
            return "block event list";
        case TimingControlKind::Invalid:
        default:
            break;
        }
        return "timing control";
    }

    void warnTimedStatement(const slang::ast::TimedStatement& stmt)
    {
        if (!diagnostics)
        {
            return;
        }
        if (stmt.timing.kind == slang::ast::TimingControlKind::EventList ||
            stmt.timing.kind == slang::ast::TimingControlKind::ImplicitEvent)
        {
            return;
        }
        std::string message("Ignoring timing control (");
        message.append(describeTiming(stmt.timing));
        message.append("); lowering statement without timing semantics");
        diagnostics->warn(stmt.sourceRange.start(), std::move(message));
    }

    void warnWaitStatement(const slang::ast::WaitStatement& stmt)
    {
        warnIgnoredStatement(stmt, "wait statement");
    }

    void warnWaitForkStatement(const slang::ast::WaitForkStatement& stmt)
    {
        warnIgnoredStatement(stmt, "wait fork");
    }

    void warnWaitOrderStatement(const slang::ast::WaitOrderStatement& stmt)
    {
        warnIgnoredStatement(stmt, "wait order");
    }

    void warnEventTriggerStatement(const slang::ast::EventTriggerStatement& stmt)
    {
        warnIgnoredStatement(stmt, "event trigger");
    }

    void warnDisableForkStatement(const slang::ast::DisableForkStatement& stmt)
    {
        warnIgnoredStatement(stmt, "disable fork");
    }

    void scanForLoopControl(const slang::ast::ForLoopStatement& stmt)
    {
        AssignmentSuppressScope suppress(*this, true);
        AssignmentExprVisitor visitor(*this);
        if (!stmt.loopVars.empty())
        {
            for (const slang::ast::VariableSymbol* var : stmt.loopVars)
            {
                if (!var)
                {
                    continue;
                }
                if (const slang::ast::Expression* initExpr = var->getInitializer())
                {
                    initExpr->visit(visitor);
                }
            }
        }
        else
        {
            for (const slang::ast::Expression* initExpr : stmt.initializers)
            {
                if (!initExpr)
                {
                    continue;
                }
                initExpr->visit(visitor);
            }
        }
        if (stmt.stopExpr)
        {
            stmt.stopExpr->visit(visitor);
        }
        for (const slang::ast::Expression* step : stmt.steps)
        {
            if (!step)
            {
                continue;
            }
            step->visit(visitor);
        }
    }

    std::vector<const slang::ast::ValueSymbol*>
    collectForLoopVars(const slang::ast::ForLoopStatement& stmt) const
    {
        std::vector<const slang::ast::ValueSymbol*> vars;
        if (!stmt.loopVars.empty())
        {
            for (const slang::ast::VariableSymbol* var : stmt.loopVars)
            {
                if (var)
                {
                    vars.push_back(var);
                }
            }
            return vars;
        }

        for (const slang::ast::Expression* initExpr : stmt.initializers)
        {
            const auto* assign = initExpr ? initExpr->as_if<slang::ast::AssignmentExpression>()
                                          : nullptr;
            if (!assign)
            {
                continue;
            }
            if (const slang::ast::ValueSymbol* symbol =
                    resolveAssignedValueSymbol(assign->left()))
            {
                vars.push_back(symbol);
            }
        }
        return vars;
    }

    bool tryUnrollRepeat(const slang::ast::RepeatLoopStatement& stmt)
    {
        if (maxLoopIterations == 0)
        {
            setLoopControlFailure("maxLoopIterations is 0");
            return false;
        }

        slang::ast::EvalContext countCtx(*plan.body);
        std::optional<int64_t> count = evalConstantInt(stmt.count, countCtx);
        if (!count || *count < 0)
        {
            setLoopControlFailure("repeat count is not statically evaluable");
            return false;
        }
        const uint64_t maxIterations = static_cast<uint64_t>(maxLoopIterations);
        if (static_cast<uint64_t>(*count) > maxIterations)
        {
            setLoopControlFailure("repeat count exceeds maxLoopIterations");
            return false;
        }

        const bool hasLoopControl = containsLoopControl(stmt.body);
        if (!hasLoopControl)
        {
            for (int64_t i = 0; i < *count; ++i)
            {
                visitStatement(stmt.body);
            }
            return true;
        }

        slang::ast::EvalContext dryCtx(*plan.body);
        LoopControlResult dryRun = runRepeatWithControl(stmt, *count, dryCtx, false);
        if (dryRun != LoopControlResult::Unsupported)
        {
            slang::ast::EvalContext emitCtx(*plan.body);
            LoopControlResult result = runRepeatWithControl(stmt, *count, emitCtx, true);
            if (result != LoopControlResult::Unsupported)
            {
                return true;
            }
        }

        clearLoopControlFailure();
        return unrollRepeatDynamic(stmt, *count);
    }

    template <typename Runner>
    bool tryUnrollWithControl(Runner runner)
    {
        if (maxLoopIterations == 0)
        {
            setLoopControlFailure("maxLoopIterations is 0");
            return false;
        }

        slang::ast::EvalContext dryCtx(*plan.body);
        LoopControlResult dryRun = runner(dryCtx, false);
        if (dryRun != LoopControlResult::Unsupported)
        {
            slang::ast::EvalContext emitCtx(*plan.body);
            LoopControlResult result = runner(emitCtx, true);
            if (result != LoopControlResult::Unsupported)
            {
                return true;
            }
        }

        return false;
    }

    bool tryUnrollWhile(const slang::ast::WhileLoopStatement& stmt)
    {
        return tryUnrollWithControl(
            [&](slang::ast::EvalContext& ctx, bool emit) {
                return runWhileWithControl(stmt, ctx, emit);
            });
    }

    bool tryUnrollDoWhile(const slang::ast::DoWhileLoopStatement& stmt)
    {
        return tryUnrollWithControl(
            [&](slang::ast::EvalContext& ctx, bool emit) {
                return runDoWhileWithControl(stmt, ctx, emit);
            });
    }

    bool tryUnrollForever(const slang::ast::ForeverLoopStatement& stmt)
    {
        return tryUnrollWithControl(
            [&](slang::ast::EvalContext& ctx, bool emit) {
                return runForeverWithControl(stmt, ctx, emit);
            });
    }

    bool tryUnrollFor(const slang::ast::ForLoopStatement& stmt)
    {
        if (!stmt.stopExpr)
        {
            setLoopControlFailure("for-loop missing stop condition");
            return false;
        }

        if (maxLoopIterations == 0)
        {
            setLoopControlFailure("maxLoopIterations is 0");
            return false;
        }

        const bool hasLoopControl = containsLoopControl(stmt.body);
        LoopLocalScope loopScope(*this, collectForLoopVars(stmt));
        if (!hasLoopControl)
        {
            slang::ast::EvalContext ctx(*plan.body);
            if (!prepareForLoopState(stmt, ctx))
            {
                return false;
            }
            syncLoopLocals(ctx);

            uint32_t iterations = 0;
            while (iterations < maxLoopIterations)
            {
                bool cond = false;
                if (!evalForLoopCondition(stmt, ctx, cond))
                {
                    return false;
                }
                if (!cond)
                {
                    return true;
                }

                syncLoopLocals(ctx);
                visitStatement(stmt.body);

                if (!executeForLoopSteps(stmt, ctx))
                {
                    return false;
                }
                syncLoopLocals(ctx);
                ++iterations;
            }
            return false;
        }

        slang::ast::EvalContext dryCtx(*plan.body);
        if (!prepareForLoopState(stmt, dryCtx))
        {
            setLoopControlFailure("for-loop init is not statically evaluable");
            return false;
        }
        LoopControlResult dryRun = runForWithControl(stmt, dryCtx, false);
        if (dryRun != LoopControlResult::Unsupported)
        {
            slang::ast::EvalContext emitCtx(*plan.body);
            if (!prepareForLoopState(stmt, emitCtx))
            {
                setLoopControlFailure("for-loop init is not statically evaluable");
                return false;
            }
            LoopControlResult result = runForWithControl(stmt, emitCtx, true);
            if (result != LoopControlResult::Unsupported)
            {
                return true;
            }
        }

        clearLoopControlFailure();
        return unrollForDynamic(stmt);
    }

    bool tryUnrollForeach(const slang::ast::ForeachLoopStatement& stmt)
    {
        if (stmt.loopDims.empty())
        {
            setLoopControlFailure("foreach has no loop dimensions");
            return false;
        }

        std::vector<const slang::ast::ValueSymbol*> loopVars;
        loopVars.reserve(stmt.loopDims.size());
        for (const auto& dim : stmt.loopDims)
        {
            if (dim.loopVar)
            {
                loopVars.push_back(dim.loopVar);
            }
        }
        LoopLocalScope loopScope(*this, std::move(loopVars));

        if (maxLoopIterations == 0)
        {
            return false;
        }

        const uint64_t maxIterations = static_cast<uint64_t>(maxLoopIterations);
        uint64_t total = 1;
        for (const auto& dim : stmt.loopDims)
        {
            if (!dim.range)
            {
                setLoopControlFailure("foreach dimension range is not static");
                return false;
            }
            const uint64_t width = dim.range->fullWidth();
            if (width == 0)
            {
                setLoopControlFailure("foreach dimension has zero width");
                return false;
            }
            if (total > maxIterations / width)
            {
                setLoopControlFailure("foreach iterations exceed maxLoopIterations");
                return false;
            }
            total *= width;
        }

        if (total > maxIterations)
        {
            setLoopControlFailure("foreach iterations exceed maxLoopIterations");
            return false;
        }

        const bool hasLoopControl = containsLoopControl(stmt.body);
        if (!hasLoopControl)
        {
            for (uint64_t i = 0; i < total; ++i)
            {
                visitStatement(stmt.body);
            }
            return true;
        }

        if (tryUnrollForeachWithControl(stmt))
        {
            return true;
        }

        clearLoopControlFailure();
        return unrollForeachDynamic(stmt, total);
    }

    bool containsLoopControl(const slang::ast::Statement& stmt) const
    {
        if (stmt.as_if<slang::ast::BreakStatement>() ||
            stmt.as_if<slang::ast::ContinueStatement>())
        {
            return true;
        }
        if (const auto* list = stmt.as_if<slang::ast::StatementList>())
        {
            for (const slang::ast::Statement* child : list->list)
            {
                if (!child)
                {
                    continue;
                }
                if (containsLoopControl(*child))
                {
                    return true;
                }
            }
            return false;
        }
        if (const auto* block = stmt.as_if<slang::ast::BlockStatement>())
        {
            return containsLoopControl(block->body);
        }
        if (const auto* timed = stmt.as_if<slang::ast::TimedStatement>())
        {
            return containsLoopControl(timed->stmt);
        }
        if (const auto* conditional = stmt.as_if<slang::ast::ConditionalStatement>())
        {
            if (containsLoopControl(conditional->ifTrue))
            {
                return true;
            }
            if (conditional->ifFalse && containsLoopControl(*conditional->ifFalse))
            {
                return true;
            }
            return false;
        }
        if (const auto* caseStmt = stmt.as_if<slang::ast::CaseStatement>())
        {
            for (const auto& item : caseStmt->items)
            {
                if (item.stmt && containsLoopControl(*item.stmt))
                {
                    return true;
                }
            }
            if (caseStmt->defaultCase && containsLoopControl(*caseStmt->defaultCase))
            {
                return true;
            }
            return false;
        }
        if (stmt.as_if<slang::ast::ForLoopStatement>() ||
            stmt.as_if<slang::ast::RepeatLoopStatement>() ||
            stmt.as_if<slang::ast::ForeachLoopStatement>() ||
            stmt.as_if<slang::ast::WhileLoopStatement>() ||
            stmt.as_if<slang::ast::DoWhileLoopStatement>() ||
            stmt.as_if<slang::ast::ForeverLoopStatement>())
        {
            return false;
        }
        if (const auto* invalid = stmt.as_if<slang::ast::InvalidStatement>())
        {
            if (invalid->child)
            {
                return containsLoopControl(*invalid->child);
            }
            return false;
        }
        return false;
    }

    LoopControlResult visitStatementWithControl(const slang::ast::Statement& stmt,
                                                slang::ast::EvalContext& ctx,
                                                bool emit)
    {
        if (!containsLoopControl(stmt))
        {
            if (emit)
            {
                visitStatement(stmt);
            }
            return LoopControlResult::None;
        }

        if (const auto* list = stmt.as_if<slang::ast::StatementList>())
        {
            for (const slang::ast::Statement* child : list->list)
            {
                if (!child)
                {
                    continue;
                }
                LoopControlResult result = visitStatementWithControl(*child, ctx, emit);
                if (result != LoopControlResult::None)
                {
                    return result;
                }
            }
            return LoopControlResult::None;
        }
        if (const auto* block = stmt.as_if<slang::ast::BlockStatement>())
        {
            return visitStatementWithControl(block->body, ctx, emit);
        }
        if (const auto* timed = stmt.as_if<slang::ast::TimedStatement>())
        {
            return visitStatementWithControl(timed->stmt, ctx, emit);
        }
        if (const auto* conditional = stmt.as_if<slang::ast::ConditionalStatement>())
        {
            return visitConditionalWithControl(*conditional, ctx, emit);
        }
        if (stmt.as_if<slang::ast::CaseStatement>())
        {
            setLoopControlFailure("case statement with break/continue is not statically evaluable");
            return LoopControlResult::Unsupported;
        }
        if (stmt.as_if<slang::ast::PatternCaseStatement>())
        {
            reportError(stmt, "Pattern case lowering is unsupported");
            return LoopControlResult::Unsupported;
        }
        if (stmt.as_if<slang::ast::BreakStatement>())
        {
            return LoopControlResult::Break;
        }
        if (stmt.as_if<slang::ast::ContinueStatement>())
        {
            return LoopControlResult::Continue;
        }
        if (const auto* exprStmt = stmt.as_if<slang::ast::ExpressionStatement>())
        {
            if (emit)
            {
                scanExpression(exprStmt->expr);
            }
            return LoopControlResult::None;
        }
        if (const auto* procAssign = stmt.as_if<slang::ast::ProceduralAssignStatement>())
        {
            if (emit)
            {
                scanExpression(procAssign->assignment);
            }
            return LoopControlResult::None;
        }
        if (const auto* invalid = stmt.as_if<slang::ast::InvalidStatement>())
        {
            if (invalid->child)
            {
                return visitStatementWithControl(*invalid->child, ctx, emit);
            }
            return LoopControlResult::None;
        }
        if (stmt.kind == slang::ast::StatementKind::Empty)
        {
            return LoopControlResult::None;
        }

        setLoopControlFailure("unsupported statement in loop with break/continue");
        return LoopControlResult::Unsupported;
    }

    LoopControlResult visitConditionalWithControl(const slang::ast::ConditionalStatement& stmt,
                                                  slang::ast::EvalContext& ctx,
                                                  bool emit)
    {
        if (stmt.conditions.empty())
        {
            reportUnsupported(stmt, "Conditional statement missing condition");
            return LoopControlResult::Unsupported;
        }
        for (const auto& cond : stmt.conditions)
        {
            if (cond.pattern)
            {
                reportError(stmt, "Patterned condition lowering is unsupported");
                return LoopControlResult::Unsupported;
            }
        }

        bool combined = true;
        for (const auto& cond : stmt.conditions)
        {
            if (emit)
            {
                scanExpression(*cond.expr);
            }
            std::optional<bool> value = evalConstantBool(*cond.expr, ctx);
            if (!value)
            {
                setLoopControlFailure("if-condition for break/continue is not statically evaluable");
                return LoopControlResult::Unsupported;
            }
            combined = combined && *value;
        }

        if (combined)
        {
            return visitStatementWithControl(stmt.ifTrue, ctx, emit);
        }
        if (stmt.ifFalse)
        {
            return visitStatementWithControl(*stmt.ifFalse, ctx, emit);
        }
        return LoopControlResult::None;
    }

    std::optional<LoopControlResult> stopOnUnsupportedOrBreak(LoopControlResult result) const
    {
        if (result == LoopControlResult::Unsupported || result == LoopControlResult::Break)
        {
            return result;
        }
        return std::nullopt;
    }

    LoopControlResult runRepeatWithControl(const slang::ast::RepeatLoopStatement& stmt,
                                           int64_t count,
                                           slang::ast::EvalContext& ctx,
                                           bool emit)
    {
        for (int64_t i = 0; i < count; ++i)
        {
            LoopControlResult result = visitStatementWithControl(stmt.body, ctx, emit);
            if (auto stop = stopOnUnsupportedOrBreak(result))
            {
                return *stop;
            }
        }
        return LoopControlResult::None;
    }

    LoopControlResult runForWithControl(const slang::ast::ForLoopStatement& stmt,
                                        slang::ast::EvalContext& ctx,
                                        bool emit)
    {
        uint32_t iterations = 0;
        while (iterations < maxLoopIterations)
        {
            bool cond = false;
            if (!evalForLoopCondition(stmt, ctx, cond))
            {
                setLoopControlFailure("for-loop condition is not statically evaluable");
                return LoopControlResult::Unsupported;
            }
            if (!cond)
            {
                return LoopControlResult::None;
            }

            syncLoopLocals(ctx);
            LoopControlResult result = visitStatementWithControl(stmt.body, ctx, emit);
            if (auto stop = stopOnUnsupportedOrBreak(result))
            {
                return *stop;
            }
            if (result == LoopControlResult::Continue)
            {
                if (!executeForLoopSteps(stmt, ctx))
                {
                    setLoopControlFailure("for-loop step is not statically evaluable");
                    return LoopControlResult::Unsupported;
                }
                syncLoopLocals(ctx);
                ++iterations;
                continue;
            }

            if (!executeForLoopSteps(stmt, ctx))
            {
                setLoopControlFailure("for-loop step is not statically evaluable");
                return LoopControlResult::Unsupported;
            }
            syncLoopLocals(ctx);
            ++iterations;
        }

        setLoopControlFailure("for-loop exceeds maxLoopIterations");
        return LoopControlResult::Unsupported;
    }

    LoopControlResult runWhileWithControl(const slang::ast::WhileLoopStatement& stmt,
                                          slang::ast::EvalContext& ctx,
                                          bool emit)
    {
        uint32_t iterations = 0;
        while (iterations < maxLoopIterations)
        {
            std::optional<bool> cond = evalConstantBool(stmt.cond, ctx);
            if (!cond)
            {
                setLoopControlFailure("while-loop condition is not statically evaluable");
                return LoopControlResult::Unsupported;
            }
            if (!*cond)
            {
                return LoopControlResult::None;
            }

            LoopControlResult result = visitStatementWithControl(stmt.body, ctx, emit);
            if (auto stop = stopOnUnsupportedOrBreak(result))
            {
                return *stop;
            }
            ++iterations;
        }

        setLoopControlFailure("while-loop exceeds maxLoopIterations");
        return LoopControlResult::Unsupported;
    }

    LoopControlResult runDoWhileWithControl(const slang::ast::DoWhileLoopStatement& stmt,
                                            slang::ast::EvalContext& ctx,
                                            bool emit)
    {
        uint32_t iterations = 0;
        while (iterations < maxLoopIterations)
        {
            LoopControlResult result = visitStatementWithControl(stmt.body, ctx, emit);
            if (auto stop = stopOnUnsupportedOrBreak(result))
            {
                return *stop;
            }
            ++iterations;

            std::optional<bool> cond = evalConstantBool(stmt.cond, ctx);
            if (!cond)
            {
                setLoopControlFailure("do-while condition is not statically evaluable");
                return LoopControlResult::Unsupported;
            }
            if (!*cond)
            {
                return LoopControlResult::None;
            }
        }

        setLoopControlFailure("do-while exceeds maxLoopIterations");
        return LoopControlResult::Unsupported;
    }

    LoopControlResult runForeverWithControl(const slang::ast::ForeverLoopStatement& stmt,
                                            slang::ast::EvalContext& ctx,
                                            bool emit)
    {
        uint32_t iterations = 0;
        while (iterations < maxLoopIterations)
        {
            LoopControlResult result = visitStatementWithControl(stmt.body, ctx, emit);
            if (auto stop = stopOnUnsupportedOrBreak(result))
            {
                return *stop;
            }
            ++iterations;
        }

        setLoopControlFailure("forever-loop exceeds maxLoopIterations");
        return LoopControlResult::Unsupported;
    }

    bool unrollRepeatDynamic(const slang::ast::RepeatLoopStatement& stmt, int64_t count)
    {
        pushLoopContext(stmt.sourceRange.start());
        for (int64_t i = 0; i < count; ++i)
        {
            ExprNodeId iterGuard = currentLoopAlive();
            pushFlowGuard(iterGuard);
            visitStatement(stmt.body);
            popFlowGuard();
        }
        popLoopContext();
        return true;
    }

    bool unrollForDynamic(const slang::ast::ForLoopStatement& stmt)
    {
        slang::ast::EvalContext ctx(*plan.body);
        if (!prepareForLoopState(stmt, ctx))
        {
            setLoopControlFailure("for-loop init is not statically evaluable");
            return false;
        }

        LoopLocalScope loopScope(*this, collectForLoopVars(stmt));
        syncLoopLocals(ctx);
        pushLoopContext(stmt.sourceRange.start());
        uint32_t iterations = 0;
        while (iterations < maxLoopIterations)
        {
            bool cond = false;
            if (!evalForLoopCondition(stmt, ctx, cond))
            {
                setLoopControlFailure("for-loop condition is not statically evaluable");
                popLoopContext();
                return false;
            }
            if (!cond)
            {
                popLoopContext();
                return true;
            }

            ExprNodeId iterGuard = currentLoopAlive();
            pushFlowGuard(iterGuard);
            syncLoopLocals(ctx);
            visitStatement(stmt.body);
            popFlowGuard();

            if (!executeForLoopSteps(stmt, ctx))
            {
                setLoopControlFailure("for-loop step is not statically evaluable");
                popLoopContext();
                return false;
            }
            syncLoopLocals(ctx);
            ++iterations;
        }

        setLoopControlFailure("for-loop exceeds maxLoopIterations");
        popLoopContext();
        return false;
    }

    bool unrollForeachDynamic(const slang::ast::ForeachLoopStatement& stmt, uint64_t total)
    {
        pushLoopContext(stmt.sourceRange.start());
        for (uint64_t i = 0; i < total; ++i)
        {
            ExprNodeId iterGuard = currentLoopAlive();
            pushFlowGuard(iterGuard);
            visitStatement(stmt.body);
            popFlowGuard();
        }
        popLoopContext();
        return true;
    }

    bool tryUnrollForeachWithControl(const slang::ast::ForeachLoopStatement& stmt)
    {
        std::vector<ForeachDimState> dims;
        dims.reserve(stmt.loopDims.size());
        for (const auto& dim : stmt.loopDims)
        {
            if (!dim.range || !dim.loopVar)
            {
                setLoopControlFailure("foreach dimension is not statically evaluable");
                return false;
            }
            const slang::ast::Type& type = dim.loopVar->getType();
            if (!type.isIntegral())
            {
                setLoopControlFailure("foreach loop variable is not integral");
                return false;
            }
            const int32_t lo = std::min(dim.range->left, dim.range->right);
            const int32_t hi = std::max(dim.range->left, dim.range->right);
            ForeachDimState state;
            state.loopVar = dim.loopVar;
            state.start = lo;
            state.stop = hi;
            state.step = 1;
            dims.push_back(state);
        }

        if (dims.empty())
        {
            return false;
        }

        slang::ast::EvalContext dryCtx(*plan.body);
        std::size_t dryIterations = 0;
        LoopControlResult dryRun =
            unrollForeachRecursive(stmt, dims, 0, dryIterations, dryCtx, false);
        if (dryRun == LoopControlResult::Unsupported)
        {
            return false;
        }

        slang::ast::EvalContext emitCtx(*plan.body);
        std::size_t emitIterations = 0;
        LoopControlResult result =
            unrollForeachRecursive(stmt, dims, 0, emitIterations, emitCtx, true);
        return result == LoopControlResult::None || result == LoopControlResult::Break;
    }

    LoopControlResult unrollForeachRecursive(const slang::ast::ForeachLoopStatement& stmt,
                                             std::span<const ForeachDimState> dims,
                                             std::size_t depth,
                                             std::size_t& iterations,
                                             slang::ast::EvalContext& ctx,
                                             bool emit)
    {
        if (depth >= dims.size())
        {
            if (iterations++ >= maxLoopIterations)
            {
                setLoopControlFailure("foreach iterations exceed maxLoopIterations");
                return LoopControlResult::Unsupported;
            }
            LoopControlResult result = visitStatementWithControl(stmt.body, ctx, emit);
            if (result == LoopControlResult::Continue)
            {
                return LoopControlResult::None;
            }
            return result;
        }

        const ForeachDimState& dim = dims[depth];
        for (int32_t index = dim.start;; index += dim.step)
        {
            if (!setLoopLocal(*dim.loopVar, index, ctx))
            {
                return LoopControlResult::Unsupported;
            }

            LoopControlResult result =
                unrollForeachRecursive(stmt, dims, depth + 1, iterations, ctx, emit);
            if (result == LoopControlResult::Break)
            {
                return LoopControlResult::Break;
            }
            if (result == LoopControlResult::Unsupported)
            {
                return LoopControlResult::Unsupported;
            }

            if (index == dim.stop)
            {
                break;
            }
        }

        return LoopControlResult::None;
    }

    bool setLoopLocal(const slang::ast::ValueSymbol& symbol, int64_t value,
                      slang::ast::EvalContext& ctx)
    {
        const slang::ast::Type& type = symbol.getType();
        if (!type.isIntegral())
        {
            return false;
        }
        const int64_t rawWidth = static_cast<int64_t>(type.getBitstreamWidth());
        const slang::bitwidth_t width =
            static_cast<slang::bitwidth_t>(rawWidth > 0 ? rawWidth : 1);
        slang::SVInt literal(value);
        slang::SVInt resized = literal.resize(width);
        resized.setSigned(type.isSigned());
        slang::ConstantValue stored(resized);
        if (slang::ConstantValue* slot = ctx.findLocal(&symbol))
        {
            *slot = std::move(stored);
            if (!loopLocalStack.empty())
            {
                loopLocalStack.back()[&symbol] = value;
            }
            return true;
        }
        if (ctx.createLocal(&symbol, std::move(stored)) != nullptr)
        {
            if (!loopLocalStack.empty())
            {
                loopLocalStack.back()[&symbol] = value;
            }
            return true;
        }
        return false;
    }

    std::optional<bool> evalConstantBool(const slang::ast::Expression& expr,
                                         slang::ast::EvalContext& ctx) const
    {
        slang::ConstantValue value = expr.eval(ctx);
        if (!value || value.hasUnknown())
        {
            return std::nullopt;
        }
        if (value.isTrue())
        {
            return true;
        }
        if (value.isFalse())
        {
            return false;
        }
        if (value.isInteger())
        {
            if (auto raw = value.integer().as<int64_t>())
            {
                return *raw != 0;
            }
        }
        return std::nullopt;
    }

    std::optional<int64_t> evalConstantInt(const slang::ast::Expression& expr,
                                           slang::ast::EvalContext& ctx) const
    {
        ctx.reset();
        slang::ConstantValue value = expr.eval(ctx);
        if (!value || !value.isInteger() || value.hasUnknown())
        {
            return std::nullopt;
        }
        return value.integer().as<int64_t>();
    }

    std::optional<slang::SVInt> evalConstantValue(const slang::ast::Expression& expr,
                                                  slang::ast::EvalContext& ctx) const
    {
        ctx.reset();
        slang::ConstantValue value = expr.eval(ctx);
        if (!value || !value.isInteger())
        {
            return std::nullopt;
        }
        return value.integer();
    }

    bool setEvalLocal(const slang::ast::ValueSymbol& symbol, int64_t value,
                      slang::ast::EvalContext& ctx) const
    {
        const slang::ast::Type& type = symbol.getType();
        if (!type.isIntegral())
        {
            return false;
        }
        const int64_t rawWidth = static_cast<int64_t>(type.getBitstreamWidth());
        const slang::bitwidth_t width =
            static_cast<slang::bitwidth_t>(rawWidth > 0 ? rawWidth : 1);
        slang::SVInt literal(value);
        slang::SVInt resized = literal.resize(width);
        resized.setSigned(type.isSigned());
        slang::ConstantValue stored(resized);
        if (slang::ConstantValue* slot = ctx.findLocal(&symbol))
        {
            *slot = std::move(stored);
            return true;
        }
        return ctx.createLocal(&symbol, std::move(stored)) != nullptr;
    }

    std::optional<int64_t> evalLoopLocalInt(const slang::ast::Expression& expr) const
    {
        if (loopLocalStack.empty())
        {
            return std::nullopt;
        }
        std::unordered_map<std::string_view, int64_t> nameValues;
        for (auto it = loopLocalStack.rbegin(); it != loopLocalStack.rend(); ++it)
        {
            for (const auto& entry : *it)
            {
                const slang::ast::ValueSymbol* symbol = entry.first;
                if (!symbol || symbol->name.empty())
                {
                    continue;
                }
                if (nameValues.find(symbol->name) == nameValues.end())
                {
                    nameValues.emplace(symbol->name, entry.second);
                }
            }
        }
        if (nameValues.empty())
        {
            return std::nullopt;
        }

        slang::ast::EvalContext ctx(*plan.body);
        struct LoopEvalVisitor
            : public slang::ast::ASTVisitor<LoopEvalVisitor, true, true> {
            LoopEvalVisitor(const StmtLowererState& state,
                            const std::unordered_map<std::string_view, int64_t>& values,
                            slang::ast::EvalContext& ctx)
                : state_(state), values_(values), ctx_(ctx)
            {
            }

            void handle(const slang::ast::NamedValueExpression& expr)
            {
                maybeSet(expr.symbol);
            }

            void handle(const slang::ast::HierarchicalValueExpression& expr)
            {
                maybeSet(expr.symbol);
            }

            void maybeSet(const slang::ast::Symbol& symbol)
            {
                const auto* valueSymbol = symbol.as_if<slang::ast::ValueSymbol>();
                if (!valueSymbol || valueSymbol->name.empty())
                {
                    return;
                }
                auto it = values_.find(valueSymbol->name);
                if (it == values_.end())
                {
                    return;
                }
                state_.setEvalLocal(*valueSymbol, it->second, ctx_);
                any_ = true;
            }

            const StmtLowererState& state_;
            const std::unordered_map<std::string_view, int64_t>& values_;
            slang::ast::EvalContext& ctx_;
            bool any_ = false;
        };

        LoopEvalVisitor visitor(*this, nameValues, ctx);
        expr.visit(visitor);
        if (!visitor.any_)
        {
            return std::nullopt;
        }

        slang::ConstantValue value = expr.eval(ctx);
        if (!value || !value.isInteger() || value.hasUnknown())
        {
            return std::nullopt;
        }
        return value.integer().as<int64_t>();
    }

    const slang::ast::ValueSymbol*
    resolveAssignedValueSymbol(const slang::ast::Expression& expr) const
    {
        const slang::ast::Expression* current = &expr;
        while (current)
        {
            if (const auto* assign = current->as_if<slang::ast::AssignmentExpression>())
            {
                current = &assign->left();
                continue;
            }
            if (const auto* named = current->as_if<slang::ast::NamedValueExpression>())
            {
                return &named->symbol;
            }
            if (const auto* hier = current->as_if<slang::ast::HierarchicalValueExpression>())
            {
                return &hier->symbol;
            }
            if (const auto* select = current->as_if<slang::ast::ElementSelectExpression>())
            {
                current = &select->value();
                continue;
            }
            if (const auto* range = current->as_if<slang::ast::RangeSelectExpression>())
            {
                current = &range->value();
                continue;
            }
            if (const auto* conversion = current->as_if<slang::ast::ConversionExpression>())
            {
                if (!conversion->isImplicit())
                {
                    break;
                }
                current = &conversion->operand();
                continue;
            }
            break;
        }
        return nullptr;
    }

    struct NetArraySelectChain {
        const slang::ast::NamedValueExpression* baseExpr = nullptr;
        const SignalInfo* signal = nullptr;
        std::vector<const slang::ast::Expression*> selectors;
    };

    std::optional<NetArraySelectChain>
    collectNetArraySelectChain(const slang::ast::Expression& expr) const
    {
        const slang::ast::Expression* current = &expr;
        std::vector<const slang::ast::Expression*> selectors;
        while (const auto* select = current->as_if<slang::ast::ElementSelectExpression>())
        {
            const slang::ast::Type* valueType = select->value().type;
            if (!valueType || !valueType->isUnpackedArray())
            {
                break;
            }
            selectors.push_back(&select->selector());
            current = &select->value();
        }
        if (selectors.empty())
        {
            return std::nullopt;
        }
        const auto* baseNamed = current->as_if<slang::ast::NamedValueExpression>();
        if (!baseNamed || baseNamed->symbol.name.empty())
        {
            return std::nullopt;
        }
        const PlanSymbolId baseId = lookupPlanSignal(plan, baseNamed->symbol);
        const SignalInfo* signal = findSignalBySymbol(plan, baseId);
        if (!signal || !isFlattenedNetArray(*signal))
        {
            return std::nullopt;
        }
        if (selectors.size() > signal->unpackedDims.size())
        {
            return std::nullopt;
        }
        std::reverse(selectors.begin(), selectors.end());
        return NetArraySelectChain{baseNamed, signal, std::move(selectors)};
    }

    bool buildNetArraySlice(const NetArraySelectChain& chain,
                            ExprNodeId& bitIndex,
                            int64_t& sliceWidth,
                            slang::SourceLocation location)
    {
        const auto& dims = chain.signal->unpackedDims;
        if (chain.selectors.empty() || dims.empty() ||
            chain.selectors.size() > dims.size())
        {
            return false;
        }

        const int64_t elementWidth = chain.signal->width > 0 ? chain.signal->width : 1;
        int64_t subarrayRows = 1;
        for (std::size_t i = chain.selectors.size(); i < dims.size(); ++i)
        {
            const int64_t extent = dims[i].extent > 0 ? dims[i].extent : 1;
            if (subarrayRows > std::numeric_limits<int64_t>::max() / extent)
            {
                subarrayRows = std::numeric_limits<int64_t>::max();
                break;
            }
            subarrayRows *= extent;
        }
        sliceWidth = elementWidth;
        if (subarrayRows > 1)
        {
            if (elementWidth > std::numeric_limits<int64_t>::max() / subarrayRows)
            {
                sliceWidth = std::numeric_limits<int64_t>::max();
            }
            else
            {
                sliceWidth = elementWidth * subarrayRows;
            }
        }
        if (sliceWidth <= 0)
        {
            return false;
        }
        const int64_t maxSliceWidth =
            static_cast<int64_t>(std::numeric_limits<int32_t>::max());
        if (sliceWidth > maxSliceWidth)
        {
            sliceWidth = maxSliceWidth;
        }

        std::vector<int64_t> strides(dims.size(), 1);
        int64_t stride = 1;
        for (std::size_t i = dims.size(); i-- > 0;)
        {
            strides[i] = stride;
            const int64_t extent = dims[i].extent > 0 ? dims[i].extent : 1;
            if (stride > std::numeric_limits<int64_t>::max() / extent)
            {
                stride = std::numeric_limits<int64_t>::max();
            }
            else
            {
                stride *= extent;
            }
        }

        ExprNodeId address = kInvalidPlanIndex;
        for (std::size_t i = 0; i < chain.selectors.size(); ++i)
        {
            auto clampIndexWidth = [](uint64_t width) -> int32_t {
                if (width == 0)
                {
                    return 0;
                }
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                return width > maxValue ? std::numeric_limits<int32_t>::max()
                                        : static_cast<int32_t>(width);
            };
            int32_t indexWidth = clampIndexWidth(computeExprWidth(*chain.selectors[i]));
            if (indexWidth <= 0)
            {
                indexWidth = 32;
            }
            ExprNodeId indexExpr = kInvalidPlanIndex;
            {
                WidthContextScope widthScope(*this, indexWidth);
                indexExpr = lowerExpression(*chain.selectors[i]);
            }
            if (indexExpr == kInvalidPlanIndex)
            {
                return false;
            }
            const auto& dim = dims[i];
            if (dim.left < dim.right)
            {
                if (dim.left != 0)
                {
                    ExprNodeId offset =
                        addConstantLiteral(std::to_string(dim.left), location);
                    indexExpr = makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                              {indexExpr, offset}, location);
                }
            }
            else
            {
                ExprNodeId offset =
                    addConstantLiteral(std::to_string(dim.left), location);
                indexExpr = makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                          {offset, indexExpr}, location);
            }
            const int64_t strideVal = strides[i];
            if (strideVal != 1)
            {
                int32_t strideWidthHint = 0;
                for (int64_t temp = strideVal; temp > 0; temp >>= 1)
                {
                    ++strideWidthHint;
                }
                if (strideWidthHint <= 0)
                {
                    strideWidthHint = 1;
                }
                int64_t mulWidthHint =
                    static_cast<int64_t>(indexWidth) + strideWidthHint;
                if (mulWidthHint > std::numeric_limits<int32_t>::max())
                {
                    mulWidthHint = std::numeric_limits<int32_t>::max();
                }
                const int32_t mulWidth = static_cast<int32_t>(mulWidthHint);
                indexExpr = resizeValueToWidth(indexExpr, mulWidth, false, location);
                ExprNodeId strideExpr =
                    addConstantLiteral(std::to_string(strideVal), location);
                strideExpr = resizeValueToWidth(strideExpr, mulWidth, false, location);
                indexExpr = makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kMul,
                                                   {indexExpr, strideExpr}, mulWidth,
                                                   location);
            }
            if (address == kInvalidPlanIndex)
            {
                address = indexExpr;
            }
            else
            {
                address = makeOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                        {address, indexExpr}, location);
            }
        }
        if (address == kInvalidPlanIndex)
        {
            return false;
        }

        bitIndex = address;
        if (elementWidth > 1)
        {
            int32_t elementWidthHint = 0;
            for (int64_t temp = elementWidth; temp > 0; temp >>= 1)
            {
                ++elementWidthHint;
            }
            if (elementWidthHint <= 0)
            {
                elementWidthHint = 1;
            }
            int32_t addressWidthHint = 0;
            if (address < lowering.values.size())
            {
                addressWidthHint = lowering.values[address].widthHint;
            }
            if (addressWidthHint <= 0)
            {
                addressWidthHint = 32;
            }
            int64_t mulWidthHint =
                static_cast<int64_t>(addressWidthHint) + elementWidthHint;
            if (mulWidthHint > std::numeric_limits<int32_t>::max())
            {
                mulWidthHint = std::numeric_limits<int32_t>::max();
            }
            const int32_t mulWidth = static_cast<int32_t>(mulWidthHint);
            bitIndex = resizeValueToWidth(address, mulWidth, false, location);
            ExprNodeId widthExpr =
                addConstantLiteral(std::to_string(elementWidth), location);
            widthExpr = resizeValueToWidth(widthExpr, mulWidth, false, location);
            bitIndex = makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kMul,
                                              {bitIndex, widthExpr}, mulWidth,
                                              location);
        }
        return true;
    }

    bool prepareForLoopState(const slang::ast::ForLoopStatement& stmt,
                             slang::ast::EvalContext& ctx) const
    {
        auto addLoopVar = [&](const slang::ast::ValueSymbol& symbol,
                              const slang::ast::Expression& initExpr) -> bool {
            const slang::ast::Type& type = symbol.getType();
            if (!type.isIntegral())
            {
                return false;
            }

            slang::ast::EvalContext initCtx(*plan.body);
            std::optional<int64_t> value = evalConstantInt(initExpr, initCtx);
            if (!value)
            {
                return false;
            }

            const int64_t rawWidth = static_cast<int64_t>(type.getBitstreamWidth());
            const slang::bitwidth_t width =
                static_cast<slang::bitwidth_t>(rawWidth > 0 ? rawWidth : 1);
            slang::SVInt initValue(*value);
            slang::SVInt sized = initValue.resize(width);
            sized.setSigned(type.isSigned());
            slang::ConstantValue initConst(sized);
            return ctx.createLocal(&symbol, std::move(initConst)) != nullptr;
        };

        if (!stmt.loopVars.empty())
        {
            for (const slang::ast::VariableSymbol* var : stmt.loopVars)
            {
                if (!var)
                {
                    continue;
                }
                const slang::ast::Expression* initExpr = var->getInitializer();
                if (!initExpr)
                {
                    return false;
                }
                if (!addLoopVar(*var, *initExpr))
                {
                    return false;
                }
            }
            return true;
        }

        if (stmt.initializers.empty())
        {
            return false;
        }
        for (const slang::ast::Expression* initExpr : stmt.initializers)
        {
            const auto* assign = initExpr ? initExpr->as_if<slang::ast::AssignmentExpression>()
                                          : nullptr;
            if (!assign)
            {
                return false;
            }
            const slang::ast::ValueSymbol* symbol = resolveAssignedValueSymbol(assign->left());
            if (!symbol)
            {
                return false;
            }
            if (!addLoopVar(*symbol, assign->right()))
            {
                return false;
            }
        }
        return true;
    }

    bool evalForLoopCondition(const slang::ast::ForLoopStatement& stmt,
                              slang::ast::EvalContext& ctx, bool& result) const
    {
        if (!stmt.stopExpr)
        {
            return false;
        }

        slang::ConstantValue cond = stmt.stopExpr->eval(ctx);
        if (!cond || cond.hasUnknown())
        {
            return false;
        }
        if (cond.isTrue())
        {
            result = true;
            return true;
        }
        if (cond.isFalse())
        {
            result = false;
            return true;
        }
        if (cond.isInteger())
        {
            auto value = cond.integer().as<int64_t>();
            if (!value)
            {
                return false;
            }
            result = *value != 0;
            return true;
        }
        return false;
    }

    bool executeForLoopSteps(const slang::ast::ForLoopStatement& stmt,
                             slang::ast::EvalContext& ctx) const
    {
        for (const slang::ast::Expression* step : stmt.steps)
        {
            if (!step)
            {
                continue;
            }
            if (const auto* assign = step->as_if<slang::ast::AssignmentExpression>())
            {
                if (assign->isCompound())
                {
                    slang::ConstantValue value = assign->eval(ctx);
                    if (!value || value.hasUnknown())
                    {
                        return false;
                    }
                    continue;
                }
                if (const slang::ast::ValueSymbol* symbol =
                        resolveAssignedValueSymbol(assign->left()))
                {
                    slang::ConstantValue value = assign->right().eval(ctx);
                    if (!value || !value.isInteger() || value.hasUnknown())
                    {
                        return false;
                    }
                    const slang::ast::Type& type = symbol->getType();
                    const int64_t rawWidth = static_cast<int64_t>(type.getBitstreamWidth());
                    const slang::bitwidth_t width =
                        static_cast<slang::bitwidth_t>(rawWidth > 0 ? rawWidth : 1);
                    slang::SVInt nextValue = value.integer().resize(width);
                    nextValue.setSigned(type.isSigned());
                    slang::ConstantValue nextConst(nextValue);
                    if (slang::ConstantValue* storage = ctx.findLocal(symbol))
                    {
                        *storage = std::move(nextConst);
                    }
                    else if (!ctx.createLocal(symbol, std::move(nextConst)))
                    {
                        return false;
                    }
                    continue;
                }
            }

            slang::ConstantValue value = step->eval(ctx);
            if (!value)
            {
                return false;
            }
        }
        return true;
    }

    bool isTwoStateExpr(const slang::ast::Expression& expr) const
    {
        const slang::ast::Expression* current = &expr;
        while (current)
        {
            if (const auto* conversion = current->as_if<slang::ast::ConversionExpression>())
            {
                if (conversion->isImplicit())
                {
                    current = &conversion->operand();
                    continue;
                }
            }
            if (const auto* named = current->as_if<slang::ast::NamedValueExpression>())
            {
                return !named->symbol.getType().isFourState();
            }
            if (const auto* hier = current->as_if<slang::ast::HierarchicalValueExpression>())
            {
                return !hier->symbol.getType().isFourState();
            }
            break;
        }
        return !expr.type->isFourState();
    }

    std::string describeFileLocation(slang::SourceLocation location) const
    {
        if (!location.valid() || !plan.body)
        {
            return {};
        }
        const auto* sourceManager = plan.body->getCompilation().getSourceManager();
        if (!sourceManager)
        {
            return {};
        }
        const auto loc = sourceManager->getFullyOriginalLoc(location);
        if (!loc.valid() || !sourceManager->isFileLoc(loc))
        {
            return {};
        }
        const auto fileName = sourceManager->getFileName(loc);
        const auto line = sourceManager->getLineNumber(loc);
        const auto column = sourceManager->getColumnNumber(loc);
        std::string text(fileName);
        text.push_back(':');
        text.append(std::to_string(line));
        text.push_back(':');
        text.append(std::to_string(column));
        return text;
    }

    ExprNodeId makeOperation(wolvrix::lib::grh::OperationKind op, std::vector<ExprNodeId> operands,
                             slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Operation;
        node.op = op;
        node.operands = std::move(operands);
        node.location = location;
        node.tempSymbol = makeTempSymbol();
        return addNode(nullptr, std::move(node));
    }

    ExprNodeId makeOperationWithWidth(wolvrix::lib::grh::OperationKind op,
                                      std::vector<ExprNodeId> operands,
                                      int32_t widthHint,
                                      slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Operation;
        node.op = op;
        node.operands = std::move(operands);
        node.location = location;
        node.tempSymbol = makeTempSymbol();
        if (widthHint > 0)
        {
            node.widthHint = widthHint;
        }
        return addNode(nullptr, std::move(node));
    }

    ExprNodeId makeSliceDynamic(ExprNodeId value, ExprNodeId index, int32_t width,
                                slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Operation;
        node.op = wolvrix::lib::grh::OperationKind::kSliceDynamic;
        node.operands = {value, index};
        node.location = location;
        node.tempSymbol = makeTempSymbol();
        node.widthHint = width > 0 ? width : 1;
        return addNode(nullptr, std::move(node));
    }

    ExprNodeId makeSliceArray(ExprNodeId value, ExprNodeId index, int32_t elementWidth,
                              slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Operation;
        node.op = wolvrix::lib::grh::OperationKind::kSliceArray;
        node.operands = {value, index};
        node.location = location;
        node.tempSymbol = makeTempSymbol();
        node.widthHint = elementWidth > 0 ? elementWidth : 1;
        node.sliceWidth = node.widthHint;
        return addNode(nullptr, std::move(node));
    }

    PlanSymbolId makeDpiResultSymbol()
    {
        return makeInternalPlanValueSymbol(plan);
    }

    ExprNodeId makeSymbolExpr(PlanSymbolId symbol, slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Symbol;
        node.symbol = symbol;
        node.location = location;
        return addNode(nullptr, std::move(node));
    }

    PlanSymbolId makeTempSymbol()
    {
        PlanSymbolId id = makeInternalPlanValueSymbol(plan);
        lowering.tempSymbols.push_back(id);
        return id;
    }

    ExprNodeId addNode(const slang::ast::Expression* expr, ExprNode node)
    {
        if (expr && expr->type && node.valueType == wolvrix::lib::grh::ValueType::Logic)
        {
            node.valueType = classifyValueType(*expr->type);
        }
        const ExprNodeId id = static_cast<ExprNodeId>(lowering.values.size());
        lowering.values.push_back(std::move(node));
        if (expr)
        {
            lowered.emplace(expr, id);
        }
        return id;
    }

    ExprNodeId addConstantLiteral(std::string literal, slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Constant;
        node.literal = std::move(literal);
        node.location = location;
        return addNode(nullptr, std::move(node));
    }

    ExprNodeId resizeValueToWidth(ExprNodeId value, int32_t targetWidth,
                                  bool signExtend, slang::SourceLocation location)
    {
        if (value == kInvalidPlanIndex || targetWidth <= 0)
        {
            return value;
        }
        int32_t sourceWidth = 0;
        if (value < lowering.values.size())
        {
            sourceWidth = lowering.values[value].widthHint;
        }
        if (sourceWidth <= 0 || sourceWidth == targetWidth)
        {
            return value;
        }
        if (targetWidth < sourceWidth)
        {
            ExprNodeId zeroIndex = addConstantLiteral("0", location);
            return makeSliceDynamic(value, zeroIndex, targetWidth, location);
        }
        const int32_t padWidth = targetWidth - sourceWidth;
        ExprNodeId pad = kInvalidPlanIndex;
        if (signExtend && sourceWidth > 0)
        {
            ExprNodeId signIndex =
                addConstantLiteral(std::to_string(sourceWidth - 1), location);
            ExprNodeId signBit = makeSliceDynamic(value, signIndex, 1, location);
            ExprNode countNode;
            countNode.kind = ExprNodeKind::Constant;
            countNode.literal = std::to_string(padWidth);
            countNode.location = location;
            countNode.widthHint = 32;
            ExprNodeId countId = addNode(nullptr, std::move(countNode));
            ExprNode repNode;
            repNode.kind = ExprNodeKind::Operation;
            repNode.op = wolvrix::lib::grh::OperationKind::kReplicate;
            repNode.operands = {countId, signBit};
            repNode.location = location;
            repNode.tempSymbol = makeTempSymbol();
            repNode.widthHint = padWidth;
            pad = addNode(nullptr, std::move(repNode));
        }
        else
        {
            ExprNode padNode;
            padNode.kind = ExprNodeKind::Constant;
            padNode.literal = std::to_string(padWidth) + "'b0";
            padNode.location = location;
            padNode.widthHint = padWidth;
            pad = addNode(nullptr, std::move(padNode));
        }
        if (pad == kInvalidPlanIndex)
        {
            return value;
        }
        ExprNode concatNode;
        concatNode.kind = ExprNodeKind::Operation;
        concatNode.op = wolvrix::lib::grh::OperationKind::kConcat;
        concatNode.operands = {pad, value};
        concatNode.location = location;
        concatNode.tempSymbol = makeTempSymbol();
        concatNode.widthHint = targetWidth;
        return addNode(nullptr, std::move(concatNode));
    }

    const slang::ast::Type* lookupSymbolType(PlanSymbolId symbol) const
    {
        if (!symbol.valid())
        {
            return nullptr;
        }
        if (auto found = proceduralLocalTypes.find(symbol.index);
            found != proceduralLocalTypes.end())
        {
            return found->second;
        }
        if (!plan.body)
        {
            return nullptr;
        }
        const std::string_view name = plan.symbolTable.text(symbol);
        if (name.empty())
        {
            return nullptr;
        }
        const slang::ast::Symbol* sym = plan.body->find(name);
        if (!sym)
        {
            return nullptr;
        }
        if (const auto* value = sym->as_if<slang::ast::ValueSymbol>())
        {
            return &value->getType();
        }
        return nullptr;
    }

    int64_t getPackedIndexOffset(const slang::ast::Type* baseType) const
    {
        if (!baseType)
        {
            return 0;
        }
        const auto& canonical = baseType->getCanonicalType();
        if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
        {
            return 0;
        }
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        return std::min<int64_t>(packed.range.left, packed.range.right);
    }

    ExprNodeId adjustPackedElementLaneIndex(const slang::ast::Type* baseType,
                                            ExprNodeId index,
                                            slang::SourceLocation location)
    {
        if (!baseType || index == kInvalidPlanIndex)
        {
            return index;
        }
        const auto& canonical = baseType->getCanonicalType();
        if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
        {
            return index;
        }
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        int32_t indexWidthHint = 0;
        if (index < lowering.values.size())
        {
            indexWidthHint = lowering.values[index].widthHint;
        }
        if (indexWidthHint <= 0)
        {
            indexWidthHint = 32;
        }

        ExprNodeId adjusted = index;
        const int64_t low = std::min<int64_t>(packed.range.left, packed.range.right);
        const int64_t high = std::max<int64_t>(packed.range.left, packed.range.right);
        if (packed.range.left <= packed.range.right)
        {
            ExprNodeId highId = addConstantLiteral(std::to_string(high), location);
            if (highId != kInvalidPlanIndex && highId < lowering.values.size())
            {
                lowering.values[highId].widthHint = indexWidthHint;
            }
            return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kSub,
                                          {highId, adjusted}, indexWidthHint,
                                          location);
        }
        if (low != 0)
        {
            ExprNodeId lowId = addConstantLiteral(std::to_string(low), location);
            if (lowId != kInvalidPlanIndex && lowId < lowering.values.size())
            {
                lowering.values[lowId].widthHint = indexWidthHint;
            }
            adjusted = makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kSub,
                                              {adjusted, lowId}, indexWidthHint,
                                              location);
        }
        return adjusted;
    }

    ExprNodeId scalePackedElementLaneIndex(ExprNodeId laneIndex, int64_t elementWidth,
                                           slang::SourceLocation location)
    {
        if (laneIndex == kInvalidPlanIndex || elementWidth <= 1)
        {
            return laneIndex;
        }
        int32_t indexWidthHint = 0;
        if (laneIndex < lowering.values.size())
        {
            indexWidthHint = lowering.values[laneIndex].widthHint;
        }
        if (indexWidthHint <= 0)
        {
            indexWidthHint = 32;
        }
        int32_t elementWidthHint = 0;
        for (int64_t temp = elementWidth; temp > 0; temp >>= 1)
        {
            ++elementWidthHint;
        }
        if (elementWidthHint <= 0)
        {
            elementWidthHint = 1;
        }
        const int32_t constWidthHint = std::max(indexWidthHint, elementWidthHint);
        int64_t mulWidthHint = static_cast<int64_t>(indexWidthHint) + elementWidthHint;
        if (mulWidthHint > std::numeric_limits<int32_t>::max())
        {
            mulWidthHint = std::numeric_limits<int32_t>::max();
        }
        ExprNodeId widthId =
            addConstantLiteral(std::to_string(elementWidth), location);
        if (widthId != kInvalidPlanIndex && widthId < lowering.values.size())
        {
            lowering.values[widthId].widthHint = constWidthHint;
        }
        return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kMul,
                                      {laneIndex, widthId},
                                      static_cast<int32_t>(mulWidthHint),
                                      location);
    }

    std::optional<std::pair<int64_t, int64_t>>
    getPackedArrayElementSlice(const slang::ast::Type* baseType) const
    {
        if (!baseType || !plan.body)
        {
            return std::nullopt;
        }
        const auto& canonical = baseType->getCanonicalType();
        if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
        {
            return std::nullopt;
        }
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        const uint64_t widthRaw =
            computeFixedWidth(packed.elementType, *plan.body, diagnostics);
        if (widthRaw == 0)
        {
            return std::nullopt;
        }
        const uint64_t totalWidthRaw =
            computeFixedWidth(canonical, *plan.body, diagnostics);
        if (totalWidthRaw <= widthRaw || widthRaw <= 1)
        {
            return std::nullopt;
        }
        const int64_t elementWidth =
            widthRaw > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
                ? static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                : static_cast<int64_t>(widthRaw);
        if (elementWidth <= 0)
        {
            return std::nullopt;
        }
        const int64_t offset = std::min<int64_t>(packed.range.left, packed.range.right);
        return std::make_pair(offset, elementWidth);
    }

    std::optional<int32_t> getPackedArrayElementWidth(const slang::ast::Type* baseType) const
    {
        auto packedElement = getPackedArrayElementSlice(baseType);
        if (!packedElement || packedElement->second <= 0)
        {
            return std::nullopt;
        }
        return packedElement->second > static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                   ? std::numeric_limits<int32_t>::max()
                   : static_cast<int32_t>(packedElement->second);
    }

    ExprNodeId applyProceduralSliceUpdate(PlanSymbolId target,
                                          const std::vector<WriteSlice>& slices,
                                          ExprNodeId value,
                                          slang::SourceLocation location)
    {
        if (!target.valid() || slices.empty() || value == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        if (slices.size() != 1)
        {
            return kInvalidPlanIndex;
        }
        const slang::ast::Type* baseType = lookupSymbolType(target);
        if (!baseType || !plan.body)
        {
            return kInvalidPlanIndex;
        }
        const uint64_t baseWidthRaw =
            computeFixedWidth(*baseType, *plan.body, diagnostics);
        if (baseWidthRaw == 0)
        {
            return kInvalidPlanIndex;
        }
        const int64_t baseWidth =
            baseWidthRaw > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
                ? static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                : static_cast<int64_t>(baseWidthRaw);
        if (baseWidth <= 0)
        {
            return kInvalidPlanIndex;
        }

        const WriteSlice& slice = slices.front();
        int64_t low = 0;
        int64_t width = 0;
        switch (slice.kind)
        {
        case WriteSliceKind::BitSelect: {
            auto indexConst = evalConstInt(plan, lowering, slice.index);
            if (!indexConst)
            {
                return kInvalidPlanIndex;
            }
            if (auto packedElement = getPackedArrayElementSlice(baseType))
            {
                low = (*indexConst - packedElement->first) * packedElement->second;
                width = packedElement->second;
            }
            else
            {
                low = *indexConst;
                width = 1;
            }
            break;
        }
        case WriteSliceKind::RangeSelect: {
            if (slice.left == kInvalidPlanIndex || slice.right == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            if (slice.rangeKind == WriteRangeKind::Simple)
            {
                auto leftConst = evalConstInt(plan, lowering, slice.left);
                auto rightConst = evalConstInt(plan, lowering, slice.right);
                if (!leftConst || !rightConst)
                {
                    return kInvalidPlanIndex;
                }
                low = std::min(*leftConst, *rightConst);
                width = std::max(*leftConst, *rightConst) - low + 1;
                break;
            }
            auto widthConst = evalConstInt(plan, lowering, slice.right);
            auto baseConst = evalConstInt(plan, lowering, slice.left);
            if (!widthConst || !baseConst || *widthConst <= 0)
            {
                return kInvalidPlanIndex;
            }
            width = *widthConst;
            if (slice.rangeKind == WriteRangeKind::IndexedUp)
            {
                low = *baseConst;
            }
            else
            {
                low = *baseConst - width + 1;
            }
            break;
        }
        default:
            return kInvalidPlanIndex;
        }

        const int64_t packedOffset = getPackedIndexOffset(baseType);
        if (packedOffset != 0 && slice.kind != WriteSliceKind::BitSelect)
        {
            low -= packedOffset;
        }
        if (width <= 0 || low < 0)
        {
            return kInvalidPlanIndex;
        }
        if (baseWidth > 0 && low + width > baseWidth)
        {
            return kInvalidPlanIndex;
        }

        ExprNodeId base = kInvalidPlanIndex;
        if (auto existing = lookupProceduralValue(target))
        {
            base = *existing;
        }
        else
        {
            ExprNode baseNode;
            baseNode.kind = ExprNodeKind::Symbol;
            baseNode.symbol = target;
            baseNode.location = location;
            base = addNode(nullptr, std::move(baseNode));
        }
        if (base == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }

        int32_t widthHint = 0;
        if (width > 0)
        {
            widthHint =
                width > static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                    ? std::numeric_limits<int32_t>::max()
                    : static_cast<int32_t>(width);
        }
        ExprNodeId sliceValue = value;
        if (widthHint > 0)
        {
            sliceValue = resizeValueToWidth(value, widthHint, false, location);
        }
        if (sliceValue == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }

        const int64_t hi = low + width - 1;
        const int64_t upperWidth = baseWidth - (hi + 1);
        std::vector<ExprNodeId> operands;
        operands.reserve(3);
        if (upperWidth > 0)
        {
            ExprNodeId upperIndex =
                addConstantLiteral(std::to_string(hi + 1), location);
            ExprNodeId upper = makeSliceDynamic(
                base, upperIndex,
                static_cast<int32_t>(std::min<int64_t>(
                    upperWidth, std::numeric_limits<int32_t>::max())),
                location);
            operands.push_back(upper);
        }
        operands.push_back(sliceValue);
        if (low > 0)
        {
            ExprNodeId lowerIndex = addConstantLiteral("0", location);
            ExprNodeId lower = makeSliceDynamic(
                base, lowerIndex,
                static_cast<int32_t>(std::min<int64_t>(
                    low, std::numeric_limits<int32_t>::max())),
                location);
            operands.push_back(lower);
        }
        if (operands.size() == 1)
        {
            return operands.front();
        }
        ExprNode concatNode;
        concatNode.kind = ExprNodeKind::Operation;
        concatNode.op = wolvrix::lib::grh::OperationKind::kConcat;
        concatNode.operands = std::move(operands);
        concatNode.location = location;
        concatNode.tempSymbol = makeTempSymbol();
        if (baseWidth > 0)
        {
            concatNode.widthHint =
                baseWidth > static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                    ? std::numeric_limits<int32_t>::max()
                    : static_cast<int32_t>(baseWidth);
        }
        return addNode(nullptr, std::move(concatNode));
    }

    static constexpr int32_t kMaxTwoStateCaseCoverageWidth = 16;

    int32_t caseControlWidth(const slang::ast::Expression& expr) const
    {
        const slang::ast::Expression* current = &expr;
        while (current)
        {
            if (const auto* conversion = current->as_if<slang::ast::ConversionExpression>())
            {
                if (conversion->isImplicit())
                {
                    current = &conversion->operand();
                    continue;
                }
            }
            if (const auto* named = current->as_if<slang::ast::NamedValueExpression>())
            {
                uint64_t width = named->symbol.getType().getBitstreamWidth();
                if (width == 0)
                {
                    if (auto effective = named->getEffectiveWidth())
                    {
                        width = *effective;
                    }
                }
                if (width == 0)
                {
                    return 0;
                }
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                return width > maxValue ? std::numeric_limits<int32_t>::max()
                                        : static_cast<int32_t>(width);
            }
            if (const auto* hier = current->as_if<slang::ast::HierarchicalValueExpression>())
            {
                uint64_t width = hier->symbol.getType().getBitstreamWidth();
                if (width == 0)
                {
                    if (auto effective = hier->getEffectiveWidth())
                    {
                        width = *effective;
                    }
                }
                if (width == 0)
                {
                    return 0;
                }
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                return width > maxValue ? std::numeric_limits<int32_t>::max()
                                        : static_cast<int32_t>(width);
            }
            break;
        }

        if (!expr.type)
        {
            return 0;
        }
        uint64_t width = expr.type->getBitstreamWidth();
        if (width == 0)
        {
            if (auto effective = expr.getEffectiveWidth())
            {
                width = *effective;
            }
        }
        if (width == 0)
        {
            return 0;
        }
        const uint64_t maxValue =
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
        return width > maxValue ? std::numeric_limits<int32_t>::max()
                                : static_cast<int32_t>(width);
    }

    bool isUnconditionalGuard(ExprNodeId guard) const
    {
        if (guard == kInvalidPlanIndex)
        {
            return true;
        }
        if (auto guardConst = evalConstInt(plan, lowering, guard))
        {
            return *guardConst != 0;
        }
        return false;
    }

    bool currentCaseCoverage() const
    {
        return !caseCoverageStack.empty() && caseCoverageStack.back();
    }

    void pushCaseCoverage(bool coversAll)
    {
        caseCoverageStack.push_back(coversAll);
    }

    void popCaseCoverage()
    {
        if (!caseCoverageStack.empty())
        {
            caseCoverageStack.pop_back();
        }
    }

    std::optional<slang::SVInt> alignCaseItemValue(const slang::ast::Expression& controlExpr,
                                                   const slang::SVInt& itemValue,
                                                   slang::bitwidth_t width) const
    {
        if (width == 0)
        {
            return std::nullopt;
        }
        const bool controlSigned = controlExpr.type && controlExpr.type->isSigned();
        const bool bothSigned = controlSigned && itemValue.isSigned();
        slang::SVInt aligned = itemValue;
        aligned.setSigned(bothSigned);
        if (width > aligned.getBitWidth())
        {
            aligned = aligned.extend(width, bothSigned);
        }
        return aligned;
    }

    std::optional<slang::SVInt> buildCaseWildcardMaskValue(
        const slang::SVInt& aligned,
        slang::ast::CaseStatementCondition condition) const
    {
        const slang::bitwidth_t width = aligned.getBitWidth();
        if (width == 0)
        {
            return std::nullopt;
        }
        std::vector<slang::logic_t> digits;
        digits.reserve(width);
        for (int32_t i = static_cast<int32_t>(width); i-- > 0;)
        {
            const slang::logic_t bit = aligned[i];
            bool wildcard = false;
            if (condition == slang::ast::CaseStatementCondition::WildcardXOrZ)
            {
                wildcard = bit.isUnknown();
            }
            else
            {
                wildcard = exactlyEqual(bit, slang::logic_t::z);
            }
            digits.push_back(wildcard ? slang::logic_t(0) : slang::logic_t(1));
        }

        return slang::SVInt::fromDigits(width, slang::LiteralBase::Binary,
                                        false, false, digits);
    }

    CaseTwoStateAnalysis analyzeCaseTwoState(const slang::ast::CaseStatement& stmt)
    {
        CaseTwoStateAnalysis info{};
        info.hasDefault = stmt.defaultCase != nullptr;

        if (stmt.condition == slang::ast::CaseStatementCondition::Inside)
        {
            return info;
        }

        const bool isWildcard =
            stmt.condition == slang::ast::CaseStatementCondition::WildcardXOrZ ||
            stmt.condition == slang::ast::CaseStatementCondition::WildcardJustZ;

        slang::ast::EvalContext ctx(*plan.body);
        if (isWildcard)
        {
            for (const auto& item : stmt.items)
            {
                for (const slang::ast::Expression* expr : item.expressions)
                {
                    if (!expr)
                    {
                        continue;
                    }
                    if (!evalConstantValue(*expr, ctx))
                    {
                        info.canLowerTwoState = false;
                        break;
                    }
                }
                if (!info.canLowerTwoState)
                {
                    break;
                }
            }
        }

        if (info.hasDefault)
        {
            return info;
        }

        const int32_t controlWidth = caseControlWidth(stmt.expr);
        if (controlWidth <= 0 || controlWidth > kMaxTwoStateCaseCoverageWidth)
        {
            return info;
        }

        struct CasePattern
        {
            uint64_t mask = 0;
            uint64_t value = 0;
        };
        std::vector<CasePattern> patterns;
        patterns.reserve(stmt.items.size());

        const slang::bitwidth_t width =
            static_cast<slang::bitwidth_t>(controlWidth);
        for (const auto& item : stmt.items)
        {
            for (const slang::ast::Expression* expr : item.expressions)
            {
                if (!expr)
                {
                    continue;
                }
                std::optional<slang::SVInt> raw = evalConstantValue(*expr, ctx);
                if (!raw)
                {
                    return info;
                }
                std::optional<slang::SVInt> alignedOpt =
                    alignCaseItemValue(stmt.expr, *raw, width);
                if (!alignedOpt)
                {
                    return info;
                }
                const slang::SVInt& aligned = *alignedOpt;

                CasePattern pattern{};
                if (!isWildcard)
                {
                    if (aligned.hasUnknown())
                    {
                        return info;
                    }
                    pattern.mask =
                        width >= 64 ? std::numeric_limits<uint64_t>::max()
                                    : ((1ULL << width) - 1ULL);
                    uint64_t value = 0;
                    for (int32_t i = 0; i < controlWidth; ++i)
                    {
                        if (exactlyEqual(aligned[i], slang::logic_t(true)))
                        {
                            value |= (1ULL << i);
                        }
                    }
                    pattern.value = value & pattern.mask;
                }
                else
                {
                    uint64_t mask = 0;
                    uint64_t value = 0;
                    for (int32_t i = 0; i < controlWidth; ++i)
                    {
                        const slang::logic_t bit = aligned[i];
                        const bool wildcard =
                            stmt.condition ==
                                    slang::ast::CaseStatementCondition::WildcardXOrZ
                                ? bit.isUnknown()
                                : exactlyEqual(bit, slang::logic_t::z);
                        if (wildcard)
                        {
                            continue;
                        }
                        if (bit.isUnknown())
                        {
                            return info;
                        }
                        mask |= (1ULL << i);
                        if (exactlyEqual(bit, slang::logic_t(true)))
                        {
                            value |= (1ULL << i);
                        }
                    }
                    pattern.mask = mask;
                    pattern.value = value & mask;
                }
                patterns.push_back(pattern);
            }
        }

        if (patterns.empty())
        {
            return info;
        }

        const uint64_t total = 1ULL << controlWidth;
        std::vector<bool> covered(total, false);
        for (uint64_t value = 0; value < total; ++value)
        {
            for (const auto& pattern : patterns)
            {
                if ((value & pattern.mask) == pattern.value)
                {
                    covered[value] = true;
                    break;
                }
            }
        }
        info.twoStateComplete =
            std::all_of(covered.begin(), covered.end(),
                        [](bool value) { return value; });
        return info;
    }

    struct CaseMaskInfo {
        ExprNodeId mask = kInvalidPlanIndex;
        ExprNodeId maskedValue = kInvalidPlanIndex;
    };

    CaseMaskInfo buildCaseWildcardMask(const slang::ast::Expression& controlExpr,
                                       const slang::ast::Expression& itemExpr,
                                       slang::ast::CaseStatementCondition condition,
                                       slang::SourceLocation location)
    {
        CaseMaskInfo info{};
        slang::ast::EvalContext ctx(*plan.body);
        std::optional<slang::SVInt> itemValue = evalConstantValue(itemExpr, ctx);
        if (!itemValue)
        {
            return info;
        }

        uint64_t controlWidthRaw = controlExpr.type->getBitstreamWidth();
        slang::bitwidth_t controlWidth =
            static_cast<slang::bitwidth_t>(controlWidthRaw > 0 ? controlWidthRaw : 0);
        slang::bitwidth_t itemWidth = itemValue->getBitWidth();
        slang::bitwidth_t width = controlWidth > 0 ? std::max(controlWidth, itemWidth) : itemWidth;
        if (width == 0)
        {
            return info;
        }

        std::optional<slang::SVInt> aligned =
            alignCaseItemValue(controlExpr, *itemValue, width);
        if (!aligned)
        {
            return info;
        }
        std::optional<slang::SVInt> mask =
            buildCaseWildcardMaskValue(*aligned, condition);
        if (!mask)
        {
            return info;
        }
        std::string literal = mask->toString(slang::LiteralBase::Binary, true);
        info.mask = addConstantLiteral(std::move(literal), location);
        slang::SVInt maskedValue = *aligned & *mask;
        std::string maskedLiteral =
            maskedValue.toString(slang::LiteralBase::Binary, true);
        info.maskedValue = addConstantLiteral(std::move(maskedLiteral), location);
        return info;
    }

    ExprNodeId buildInsideValueRangeMatch(ExprNodeId control,
                                          const slang::ast::ValueRangeExpression& range,
                                          slang::SourceLocation location)
    {
        const bool leftUnbounded =
            range.left().kind == slang::ast::ExpressionKind::UnboundedLiteral;
        const bool rightUnbounded =
            range.right().kind == slang::ast::ExpressionKind::UnboundedLiteral;

        ExprNodeId left = kInvalidPlanIndex;
        ExprNodeId right = kInvalidPlanIndex;
        if (!leftUnbounded)
        {
            left = lowerExpression(range.left());
            if (left == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
        }
        if (!rightUnbounded)
        {
            right = lowerExpression(range.right());
            if (right == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
        }

        if (range.rangeKind == slang::ast::ValueRangeKind::Simple)
        {
            ExprNodeId lowerBound = kInvalidPlanIndex;
            ExprNodeId upperBound = kInvalidPlanIndex;
            if (!leftUnbounded)
            {
                lowerBound =
                    makeOperation(wolvrix::lib::grh::OperationKind::kGe, {control, left}, location);
            }
            if (!rightUnbounded)
            {
                upperBound =
                    makeOperation(wolvrix::lib::grh::OperationKind::kLe, {control, right}, location);
            }
            if (lowerBound == kInvalidPlanIndex && upperBound == kInvalidPlanIndex)
            {
                return addConstantLiteral("1'b1", location);
            }
            if (lowerBound == kInvalidPlanIndex)
            {
                return upperBound;
            }
            if (upperBound == kInvalidPlanIndex)
            {
                return lowerBound;
            }
            return makeLogicAnd(lowerBound, upperBound, location);
        }

        if (leftUnbounded || rightUnbounded)
        {
            reportUnsupported(range, "Unbounded inside tolerance range unsupported");
            return kInvalidPlanIndex;
        }

        ExprNodeId tolerance = right;
        if (range.rangeKind == slang::ast::ValueRangeKind::RelativeTolerance)
        {
            const bool useFloating =
                (range.left().type && range.left().type->isFloating()) ||
                (range.right().type && range.right().type->isFloating());
            ExprNodeId scale =
                addConstantLiteral(useFloating ? "100.0" : "100", location);
            ExprNodeId mul =
                makeOperation(wolvrix::lib::grh::OperationKind::kMul, {left, right}, location);
            tolerance = makeOperation(wolvrix::lib::grh::OperationKind::kDiv, {mul, scale}, location);
        }
        else if (range.rangeKind != slang::ast::ValueRangeKind::AbsoluteTolerance)
        {
            reportUnsupported(range, "Unsupported inside tolerance range kind");
            return kInvalidPlanIndex;
        }

        ExprNodeId lowerExpr =
            makeOperation(wolvrix::lib::grh::OperationKind::kSub, {left, tolerance}, location);
        ExprNodeId upperExpr =
            makeOperation(wolvrix::lib::grh::OperationKind::kAdd, {left, tolerance}, location);
        ExprNodeId lowerBound =
            makeOperation(wolvrix::lib::grh::OperationKind::kGe, {control, lowerExpr}, location);
        ExprNodeId upperBound =
            makeOperation(wolvrix::lib::grh::OperationKind::kLe, {control, upperExpr}, location);
        return makeLogicAnd(lowerBound, upperBound, location);
    }

    ExprNodeId buildInsideItemMatch(ExprNodeId control,
                                    const slang::ast::Expression& controlExpr,
                                    const slang::ast::Expression& item,
                                    slang::SourceLocation location)
    {
        if (const auto* range = item.as_if<slang::ast::ValueRangeExpression>())
        {
            return buildInsideValueRangeMatch(control, *range, location);
        }
        if (item.kind == slang::ast::ExpressionKind::UnboundedLiteral)
        {
            reportUnsupported(item, "Unbounded literal inside match unsupported");
            return kInvalidPlanIndex;
        }

        ExprNodeId itemId = lowerExpression(item);
        if (itemId == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }

        const bool integral = controlExpr.type && controlExpr.type->isIntegral() &&
                              item.type && item.type->isIntegral();
        const wolvrix::lib::grh::OperationKind op =
            integral ? wolvrix::lib::grh::OperationKind::kWildcardEq
                     : wolvrix::lib::grh::OperationKind::kEq;
        return makeOperation(op, {control, itemId}, location);
    }

    ExprNodeId buildCaseItemMatch(ExprNodeId control,
                                  const slang::ast::Expression& controlExpr,
                                  slang::ast::CaseStatementCondition condition,
                                  std::span<const slang::ast::Expression* const> items,
                                  slang::SourceLocation location,
                                  CaseLoweringMode mode)
    {
        if (control == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        const int32_t controlWidth = caseControlWidth(controlExpr);
        if (condition == slang::ast::CaseStatementCondition::Inside)
        {
            ExprNodeId combined = kInvalidPlanIndex;
            for (const slang::ast::Expression* expr : items)
            {
                if (!expr)
                {
                    continue;
                }
                scanExpression(*expr);
                ExprNodeId term =
                    buildInsideItemMatch(control, controlExpr, *expr, location);
                if (term == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                if (combined == kInvalidPlanIndex)
                {
                    combined = term;
                }
                else
                {
                    combined = makeLogicOr(combined, term, location);
                }
            }
            return combined;
        }
        ExprNodeId combined = kInvalidPlanIndex;
        for (const slang::ast::Expression* expr : items)
        {
            if (!expr)
            {
                continue;
            }
            scanExpression(*expr);
            ExprNodeId itemId = lowerExpression(*expr);
            if (itemId == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            if (controlWidth > 0 && itemId < lowering.values.size() &&
                isIntegerLiteralExpr(*expr))
            {
                int32_t& hint = lowering.values[itemId].widthHint;
                if (hint <= 0 || hint < controlWidth)
                {
                    hint = controlWidth;
                }
            }

            ExprNodeId term = kInvalidPlanIndex;
            if (condition == slang::ast::CaseStatementCondition::WildcardXOrZ ||
                condition == slang::ast::CaseStatementCondition::WildcardJustZ)
            {
                CaseMaskInfo maskInfo =
                    buildCaseWildcardMask(controlExpr, *expr, condition, location);
                if (maskInfo.mask == kInvalidPlanIndex ||
                    maskInfo.maskedValue == kInvalidPlanIndex)
                {
                    reportUnsupported(*expr,
                                      "Wildcard case item not constant; fallback to case equality");
                    term = mode == CaseLoweringMode::FourState
                               ? makeCaseEq(control, itemId, location)
                               : makeEq(control, itemId, location);
                }
                else
                {
                    ExprNodeId maskedControl =
                        makeOperation(wolvrix::lib::grh::OperationKind::kAnd, {control, maskInfo.mask},
                                      location);
                    const wolvrix::lib::grh::OperationKind eqOp =
                        mode == CaseLoweringMode::FourState
                            ? wolvrix::lib::grh::OperationKind::kCaseEq
                            : wolvrix::lib::grh::OperationKind::kEq;
                    term = makeOperation(eqOp, {maskedControl, maskInfo.maskedValue}, location);
                }
            }
            else
            {
                term = mode == CaseLoweringMode::FourState
                           ? makeCaseEq(control, itemId, location)
                           : makeEq(control, itemId, location);
            }

            if (combined == kInvalidPlanIndex)
            {
                combined = term;
            }
            else
            {
                combined = makeLogicOr(combined, term, location);
            }
        }
        return combined;
    }

    ExprNodeId lowerExpression(const slang::ast::Expression& expr)
    {
        const bool usesLoopLocal = exprUsesLoopLocal(expr);
        const bool usesProcedural = exprUsesProceduralValue(expr);
        const bool cacheable =
            !(usesLoopLocal || usesProcedural || widthContext > 0);
        auto addNodeForExpr = [&](ExprNode node) -> ExprNodeId {
            return addNode(cacheable ? &expr : nullptr, std::move(node));
        };
        if (cacheable)
        {
            if (auto it = lowered.find(&expr); it != lowered.end())
            {
                return it->second;
            }
        }

        auto adjustPackedIndex =
            [&](const slang::ast::Type* baseType, ExprNodeId index,
                slang::SourceLocation location) -> ExprNodeId {
            if (!baseType || index == kInvalidPlanIndex)
            {
                return index;
            }
            const auto& canonical = baseType->getCanonicalType();
            if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
            {
                return index;
            }
            const auto& packed = canonical.as<slang::ast::PackedArrayType>();
            const int64_t offset =
                std::min<int64_t>(packed.range.left, packed.range.right);
            uint64_t elementWidthRaw = packed.elementType.getBitstreamWidth();
            if (elementWidthRaw == 0)
            {
                elementWidthRaw = 1;
            }
            constexpr uint64_t maxWidth =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            const int64_t elementWidth =
                elementWidthRaw > maxWidth ? static_cast<int64_t>(maxWidth)
                                           : static_cast<int64_t>(elementWidthRaw);
            int32_t indexWidthHint = 0;
            if (index < lowering.values.size())
            {
                indexWidthHint = lowering.values[index].widthHint;
            }
            if (indexWidthHint <= 0)
            {
                indexWidthHint = 32;
            }
            ExprNodeId adjustedId = index;
            if (offset != 0)
            {
                ExprNode offsetNode;
                offsetNode.kind = ExprNodeKind::Constant;
                offsetNode.literal = std::to_string(offset);
                offsetNode.location = location;
                offsetNode.widthHint = indexWidthHint;
                ExprNodeId offsetId = addNode(nullptr, std::move(offsetNode));
                ExprNode subNode;
                subNode.kind = ExprNodeKind::Operation;
                subNode.op = wolvrix::lib::grh::OperationKind::kSub;
                subNode.operands = {adjustedId, offsetId};
                subNode.location = location;
                subNode.tempSymbol = makeTempSymbol();
                subNode.widthHint = indexWidthHint;
                adjustedId = addNode(nullptr, std::move(subNode));
            }
            if (elementWidth > 1)
            {
                int32_t elementWidthHint = 0;
                for (int64_t temp = elementWidth; temp > 0; temp >>= 1)
                {
                    ++elementWidthHint;
                }
                if (elementWidthHint <= 0)
                {
                    elementWidthHint = 1;
                }
                const int32_t constWidthHint =
                    elementWidthHint > indexWidthHint ? elementWidthHint : indexWidthHint;
                int64_t mulWidthHint =
                    static_cast<int64_t>(indexWidthHint) + elementWidthHint;
                if (mulWidthHint > std::numeric_limits<int32_t>::max())
                {
                    mulWidthHint = std::numeric_limits<int32_t>::max();
                }
                ExprNode widthNode;
                widthNode.kind = ExprNodeKind::Constant;
                widthNode.literal = std::to_string(elementWidth);
                widthNode.location = location;
                widthNode.widthHint = constWidthHint;
                ExprNodeId widthId = addNode(nullptr, std::move(widthNode));
                const int32_t mulWidth = static_cast<int32_t>(mulWidthHint);
                adjustedId = resizeValueToWidth(adjustedId, mulWidth, false,
                                                location);
                widthId = resizeValueToWidth(widthId, mulWidth, false,
                                             location);
                ExprNode mulNode;
                mulNode.kind = ExprNodeKind::Operation;
                mulNode.op = wolvrix::lib::grh::OperationKind::kMul;
                mulNode.operands = {adjustedId, widthId};
                mulNode.location = location;
                mulNode.tempSymbol = makeTempSymbol();
                mulNode.widthHint = mulWidth;
                adjustedId = addNode(nullptr, std::move(mulNode));
            }
            // Clamp packed-array bit indices to the addressing width of the bitstream.
            uint64_t bitstreamWidth = canonical.getBitstreamWidth();
            if (bitstreamWidth > 1)
            {
                uint64_t temp = bitstreamWidth - 1;
                int32_t clampWidth = 0;
                while (temp > 0 && clampWidth < std::numeric_limits<int32_t>::max())
                {
                    ++clampWidth;
                    temp >>= 1;
                }
                if (clampWidth > 0)
                {
                    adjustedId =
                        resizeValueToWidth(adjustedId, clampWidth, false, location);
                }
            }
            return adjustedId;
        };

        auto applyConversion =
            [&](ExprNodeId value, const slang::ast::Type* targetType,
                slang::SourceLocation location) -> ExprNodeId {
            if (value == kInvalidPlanIndex || !targetType)
            {
                return value;
            }
            uint64_t widthRaw = targetType->getBitstreamWidth();
            if (widthRaw == 0)
            {
                return value;
            }
            const uint64_t maxValue =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            const int32_t targetWidth =
                widthRaw > maxValue ? std::numeric_limits<int32_t>::max()
                                    : static_cast<int32_t>(widthRaw);
            if (targetWidth <= 0)
            {
                return value;
            }
            int32_t sourceWidth = 0;
            if (value < lowering.values.size())
            {
                sourceWidth = lowering.values[value].widthHint;
            }
            if (sourceWidth <= 0)
            {
                sourceWidth = targetWidth;
            }
            if (sourceWidth == targetWidth)
            {
                return value;
            }
            if (targetWidth < sourceWidth)
            {
                ExprNodeId zeroIndex = addConstantLiteral("0", location);
                return makeSliceDynamic(value, zeroIndex, targetWidth, location);
            }
            const int32_t padWidth = targetWidth - sourceWidth;
            ExprNodeId pad = kInvalidPlanIndex;
            if (targetType->isSigned() && sourceWidth > 0)
            {
                ExprNodeId signIndex =
                    addConstantLiteral(std::to_string(sourceWidth - 1), location);
                ExprNodeId signBit = makeSliceDynamic(value, signIndex, 1, location);
                ExprNode countNode;
                countNode.kind = ExprNodeKind::Constant;
                countNode.literal = std::to_string(padWidth);
                countNode.location = location;
                countNode.widthHint = 32;
                ExprNodeId countId = addNode(nullptr, std::move(countNode));
                ExprNode repNode;
                repNode.kind = ExprNodeKind::Operation;
                repNode.op = wolvrix::lib::grh::OperationKind::kReplicate;
                repNode.operands = {countId, signBit};
                repNode.location = location;
                repNode.tempSymbol = makeTempSymbol();
                repNode.widthHint = padWidth;
                pad = addNode(nullptr, std::move(repNode));
            }
            else
            {
                ExprNode padNode;
                padNode.kind = ExprNodeKind::Constant;
                padNode.literal = std::to_string(padWidth) + "'b0";
                padNode.location = location;
                padNode.widthHint = padWidth;
                pad = addNode(nullptr, std::move(padNode));
            }
            if (pad == kInvalidPlanIndex)
            {
                return value;
            }
            ExprNode concatNode;
            concatNode.kind = ExprNodeKind::Operation;
            concatNode.op = wolvrix::lib::grh::OperationKind::kConcat;
            concatNode.operands = {pad, value};
            concatNode.location = location;
            concatNode.tempSymbol = makeTempSymbol();
            concatNode.widthHint = targetWidth;
            return addNode(nullptr, std::move(concatNode));
        };

        auto paramLiteral = [](const slang::ast::ParameterSymbol& param)
            -> std::optional<std::string> {
            slang::ConstantValue value = param.getValue();
            if (value.bad())
            {
                return std::nullopt;
            }
            if (!value.isInteger())
            {
                value = value.convertToInt();
            }
            if (!value.isInteger())
            {
                return std::nullopt;
            }
            const slang::SVInt& literal = value.integer();
            if (literal.hasUnknown())
            {
                return std::nullopt;
            }
            return formatIntegerLiteral(literal);
        };

        ExprNode node;
        node.location = expr.sourceRange.start();

        auto applyExprWidthHint = [&](ExprNode& out) {
            if (out.widthHint != 0)
            {
                return;
            }
            uint64_t width = computeExprWidth(expr);
            if (width > 0)
            {
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                out.widthHint = width > maxValue
                                    ? std::numeric_limits<int32_t>::max()
                                    : static_cast<int32_t>(width);
            }
        };

        if (auto loopConst = evalLoopLocalInt(expr))
        {
            node.kind = ExprNodeKind::Constant;
            node.literal = std::to_string(*loopConst);
            uint64_t width = computeExprWidth(expr);
            if (width > 0)
            {
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                node.widthHint = width > maxValue
                                     ? std::numeric_limits<int32_t>::max()
                                     : static_cast<int32_t>(width);
            }
            return addNodeForExpr(std::move(node));
        }

        if (!usesLoopLocal)
        {
            if (const slang::ConstantValue* constant = expr.getConstant())
            {
                if (constant->isInteger())
                {
                    const slang::SVInt& literal = constant->integer();
                    if (!literal.hasUnknown())
                    {
                        node.kind = ExprNodeKind::Constant;
                        node.literal = formatIntegerLiteral(literal);
                        node.isSigned = expr.type ? expr.type->isSigned()
                                                  : literal.isSigned();
                        applyExprWidthHint(node);
                        return addNodeForExpr(std::move(node));
                    }
                }
            }
        }

        if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
        {
            if (auto procValue = lookupProceduralValue(named->symbol))
            {
                return *procValue;
            }
            if (auto loopValue = lookupLoopLocal(named->symbol))
            {
                node.kind = ExprNodeKind::Constant;
                node.literal = std::to_string(*loopValue);
                uint64_t width = named->symbol.getType().getBitstreamWidth();
                if (width > 0)
                {
                    const uint64_t maxValue =
                        static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                    node.widthHint = width > maxValue
                                         ? std::numeric_limits<int32_t>::max()
                                         : static_cast<int32_t>(width);
                }
                return addNodeForExpr(std::move(node));
            }
            if (const auto* param =
                    named->symbol.as_if<slang::ast::ParameterSymbol>())
            {
                if (auto literal = paramLiteral(*param))
                {
                    node.kind = ExprNodeKind::Constant;
                    node.literal = *literal;
                    applyExprWidthHint(node);
                    return addNodeForExpr(std::move(node));
                }
            }
            node.kind = ExprNodeKind::Symbol;
            if (auto local = lookupProceduralLocal(named->symbol))
            {
                node.symbol = *local;
            }
            else
            {
                node.symbol = lookupPlanSignal(plan, named->symbol);
            }
            if (!node.symbol.valid() &&
                (named->symbol.kind == slang::ast::SymbolKind::Parameter ||
                 named->symbol.kind == slang::ast::SymbolKind::TypeParameter))
            {
                node.symbol = plan.symbolTable.intern(named->symbol.name);
            }
            if (!node.symbol.valid())
            {
                reportUnsupported(expr, "Unknown symbol in expression");
            }
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* hier = expr.as_if<slang::ast::HierarchicalValueExpression>())
        {
            if (const auto* param =
                    hier->symbol.as_if<slang::ast::ParameterSymbol>())
            {
                if (auto literal = paramLiteral(*param))
                {
                    node.kind = ExprNodeKind::Constant;
                    node.literal = *literal;
                    applyExprWidthHint(node);
                    return addNodeForExpr(std::move(node));
                }
            }
            auto path = extractXmrPath(*hier);
            if (!path)
            {
                reportError(expr, "Unsupported hierarchical symbol in expression");
                return kInvalidPlanIndex;
            }
            node.kind = ExprNodeKind::XmrRead;
            node.xmrPath = std::move(*path);
            node.isSigned = expr.type ? expr.type->isSigned() : hier->symbol.getType().isSigned();
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* literal = expr.as_if<slang::ast::IntegerLiteral>())
        {
            node.kind = ExprNodeKind::Constant;
            node.literal = formatIntegerLiteral(literal->getValue());
            node.isSigned = expr.type ? expr.type->isSigned()
                                      : literal->getValue().isSigned();
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* literal = expr.as_if<slang::ast::UnbasedUnsizedIntegerLiteral>())
        {
            node.kind = ExprNodeKind::Constant;
            node.literal = formatIntegerLiteral(literal->getValue());
            node.isSigned = expr.type ? expr.type->isSigned()
                                      : literal->getValue().isSigned();
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* literal = expr.as_if<slang::ast::RealLiteral>())
        {
            node.kind = ExprNodeKind::Constant;
            std::ostringstream oss;
            oss << literal->getValue();
            node.literal = oss.str();
            return addNodeForExpr(std::move(node));
        }
        if (const auto* literal = expr.as_if<slang::ast::StringLiteral>())
        {
            node.kind = ExprNodeKind::Constant;
            node.literal.assign(literal->getValue());
            node.valueType = wolvrix::lib::grh::ValueType::String;
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* call = expr.as_if<slang::ast::CallExpression>())
        {
            if (call->isSystemCall())
            {
                const std::string name =
                    normalizeSystemTaskName(call->getSubroutineName());
                if (name == "bits")
                {
                    const auto args = call->arguments();
                    const slang::ast::Expression* arg =
                        (!args.empty() ? args.front() : nullptr);
                    if (arg && arg->type)
                    {
                        uint64_t width = arg->type->getBitstreamWidth();
                        if (width == 0)
                        {
                            if (auto effective = arg->getEffectiveWidth())
                            {
                                width = *effective;
                            }
                        }
                        if (width > 0)
                        {
                            node.kind = ExprNodeKind::Constant;
                            node.literal = std::to_string(width);
                            return addNodeForExpr(std::move(node));
                        }
                    }
                    reportUnsupported(expr, "$bits argument width is unknown");
                    return kInvalidPlanIndex;
                }
                if (name == "unsigned" || name == "signed")
                {
                    const auto args = call->arguments();
                    const slang::ast::Expression* arg =
                        (!args.empty() ? args.front() : nullptr);
                    if (!arg)
                    {
                        reportUnsupported(expr, "$unsigned/$signed missing argument");
                        return kInvalidPlanIndex;
                    }
                    if (const slang::ConstantValue* constant = arg->getConstant())
                    {
                        if (constant->isInteger())
                        {
                            const slang::SVInt& literal = constant->integer();
                            if (!literal.hasUnknown())
                            {
                                ExprNode constNode;
                                constNode.kind = ExprNodeKind::Constant;
                                constNode.literal = formatIntegerLiteral(literal);
                                constNode.location = expr.sourceRange.start();
                                constNode.isSigned = (name == "signed");
                                applyExprWidthHint(constNode);
                                return addNodeForExpr(std::move(constNode));
                            }
                        }
                    }
                    ExprNodeId operand = kInvalidPlanIndex;
                    {
                        WidthContextScope widthScope(*this, 0);
                        operand = lowerExpression(*arg);
                    }
                    if (operand == kInvalidPlanIndex)
                    {
                        return kInvalidPlanIndex;
                    }
                    ExprNodeId converted = operand;
                    if (expr.type)
                    {
                        converted = applyConversion(operand, expr.type, expr.sourceRange.start());
                        if (converted == kInvalidPlanIndex)
                        {
                            return kInvalidPlanIndex;
                        }
                    }
                    ExprNode castNode;
                    castNode.kind = ExprNodeKind::Operation;
                    castNode.op = wolvrix::lib::grh::OperationKind::kAssign;
                    castNode.operands = {converted};
                    castNode.location = expr.sourceRange.start();
                    castNode.tempSymbol = makeTempSymbol();
                    castNode.isSigned = (name == "signed");
                    if (expr.type)
                    {
                        uint64_t width = expr.type->getBitstreamWidth();
                        if (width > 0)
                        {
                            const uint64_t maxValue =
                                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                            castNode.widthHint = width > maxValue
                                                     ? std::numeric_limits<int32_t>::max()
                                                     : static_cast<int32_t>(width);
                        }
                    }
                    return addNodeForExpr(std::move(castNode));
                }
                if (name == "time" || name == "stime" || name == "realtime" ||
                    name == "random" || name == "urandom" || name == "urandom_range" ||
                    name == "fopen" || name == "ferror" || name == "clog2" || name == "size")
                {
                    const auto args = call->arguments();
                    if (name == "time" || name == "stime" || name == "realtime")
                    {
                        if (!args.empty())
                        {
                            reportUnsupported(expr, "$time/$stime/$realtime do not take arguments");
                            return kInvalidPlanIndex;
                        }
                    }
                    else if (name == "random" || name == "urandom")
                    {
                        if (args.size() > 1)
                        {
                            reportUnsupported(expr, "$random/$urandom accept at most one argument");
                            return kInvalidPlanIndex;
                        }
                    }
                    else if (name == "urandom_range")
                    {
                        if (args.empty() || args.size() > 2)
                        {
                            reportUnsupported(expr, "$urandom_range expects one or two arguments");
                            return kInvalidPlanIndex;
                        }
                    }
                    else if (name == "fopen")
                    {
                        if (args.empty() || args.size() > 2)
                        {
                            reportUnsupported(expr, "$fopen expects one or two arguments");
                            return kInvalidPlanIndex;
                        }
                    }
                    else if (name == "ferror")
                    {
                        if (args.size() != 1)
                        {
                            reportUnsupported(expr, "$ferror supports only one argument");
                            return kInvalidPlanIndex;
                        }
                    }
                    else if (name == "clog2" || name == "size")
                    {
                        if (args.size() != 1)
                        {
                            reportUnsupported(expr, "$clog2/$size expect exactly one argument");
                            return kInvalidPlanIndex;
                        }
                    }

                    if (name == "clog2")
                    {
                        const slang::ast::Expression* arg =
                            (!args.empty() ? args.front() : nullptr);
                        if (arg)
                        {
                            if (const slang::ConstantValue* constant = arg->getConstant())
                            {
                                if (constant->isInteger())
                                {
                                    const slang::SVInt& literal = constant->integer();
                                    if (!literal.hasUnknown())
                                    {
                                        const uint32_t result = slang::clog2(literal);
                                        ExprNode constNode;
                                        constNode.kind = ExprNodeKind::Constant;
                                        constNode.literal = std::to_string(result);
                                        constNode.location = expr.sourceRange.start();
                                        applyExprWidthHint(constNode);
                                        return addNodeForExpr(std::move(constNode));
                                    }
                                }
                            }
                        }
                    }
                    if (name == "size")
                    {
                        const slang::ast::Expression* arg =
                            (!args.empty() ? args.front() : nullptr);
                        if (arg && arg->type && arg->type->hasFixedRange())
                        {
                            const slang::ConstantRange range = arg->type->getFixedRange();
                            const uint64_t width = range.fullWidth();
                            ExprNode constNode;
                            constNode.kind = ExprNodeKind::Constant;
                            constNode.literal = std::to_string(width);
                            constNode.location = expr.sourceRange.start();
                            applyExprWidthHint(constNode);
                            return addNodeForExpr(std::move(constNode));
                        }
                    }

                    ExprNode sysNode;
                    sysNode.kind = ExprNodeKind::Operation;
                    sysNode.op = wolvrix::lib::grh::OperationKind::kSystemFunction;
                    sysNode.systemName = name;
                    sysNode.location = expr.sourceRange.start();
                    sysNode.tempSymbol = makeTempSymbol();
                    sysNode.isSigned = expr.type ? expr.type->isSigned() : false;
                    sysNode.hasSideEffects =
                        (name == "random" || name == "urandom" || name == "urandom_range" ||
                         name == "fopen" || name == "ferror");
                    sysNode.procKind = eventContext.procKind;
                    sysNode.hasTiming = eventContext.hasTiming;

                    for (const auto* arg : args)
                    {
                        if (!arg)
                        {
                            reportUnsupported(expr, "System function argument is missing");
                            return kInvalidPlanIndex;
                        }
                        ExprNodeId argId = lowerExpression(*arg);
                        if (argId == kInvalidPlanIndex)
                        {
                            reportUnsupported(expr, "Failed to lower system function argument");
                            return kInvalidPlanIndex;
                        }
                        sysNode.operands.push_back(argId);
                    }

                    applyExprWidthHint(sysNode);
                    if (sysNode.widthHint == 0)
                    {
                        if (name == "time" || name == "realtime")
                        {
                            sysNode.widthHint = 64;
                        }
                        else
                        {
                            sysNode.widthHint = 32;
                        }
                    }
                    return addNodeForExpr(std::move(sysNode));
                }
                if (name == "sformatf" || name == "psprintf" ||
                    name == "itor" || name == "rtoi" ||
                    name == "realtobits" || name == "bitstoreal")
                {
                    const auto args = call->arguments();
                    if ((name == "itor" || name == "rtoi" ||
                         name == "realtobits" || name == "bitstoreal") &&
                        args.size() != 1)
                    {
                        reportUnsupported(expr, "System function expects one argument");
                        return kInvalidPlanIndex;
                    }
                    if ((name == "sformatf" || name == "psprintf") && args.empty())
                    {
                        reportUnsupported(expr, "System function expects at least one argument");
                        return kInvalidPlanIndex;
                    }

                    ExprNode sysNode;
                    sysNode.kind = ExprNodeKind::Operation;
                    sysNode.op = wolvrix::lib::grh::OperationKind::kSystemFunction;
                    sysNode.systemName = name;
                    sysNode.location = expr.sourceRange.start();
                    sysNode.tempSymbol = makeTempSymbol();
                    sysNode.isSigned = expr.type ? expr.type->isSigned() : false;
                    sysNode.procKind = eventContext.procKind;
                    sysNode.hasTiming = eventContext.hasTiming;

                    for (const auto* arg : args)
                    {
                        if (!arg)
                        {
                            reportUnsupported(expr, "System function argument is missing");
                            return kInvalidPlanIndex;
                        }
                        ExprNodeId argId = lowerExpression(*arg);
                        if (argId == kInvalidPlanIndex)
                        {
                            reportUnsupported(expr, "Failed to lower system function argument");
                            return kInvalidPlanIndex;
                        }
                        sysNode.operands.push_back(argId);
                    }

                    applyExprWidthHint(sysNode);
                    return addNodeForExpr(std::move(sysNode));
                }
            }
            if (auto* subroutine = std::get_if<const slang::ast::SubroutineSymbol*>(&call->subroutine))
            {
                if (*subroutine && (*subroutine)->flags.has(slang::ast::MethodFlags::DPIImport))
                {
                    ExprNodeId dpiResult = handleDpiCallExpr(*call, **subroutine,
                                                             cacheable ? &expr : nullptr);
                    if (dpiResult != kInvalidPlanIndex)
                    {
                        return dpiResult;
                    }
                }
            }
        }
        if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
        {
            ExprNodeId operand = kInvalidPlanIndex;
            if (conversion->isImplicit())
            {
                operand = lowerExpression(conversion->operand());
            }
            else
            {
                WidthContextScope widthScope(*this, 0);
                operand = lowerExpression(conversion->operand());
            }
            ExprNodeId converted =
                applyConversion(operand, conversion->type, expr.sourceRange.start());
            if (converted == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            if (!conversion->isImplicit())
            {
                ExprNode castNode;
                castNode.kind = ExprNodeKind::Operation;
                castNode.op = wolvrix::lib::grh::OperationKind::kAssign;
                castNode.operands = {converted};
                castNode.location = expr.sourceRange.start();
                castNode.tempSymbol = makeTempSymbol();
                castNode.isSigned = conversion->type ? conversion->type->isSigned() : false;
                if (conversion->type)
                {
                    uint64_t width = conversion->type->getBitstreamWidth();
                    if (width > 0)
                    {
                        const uint64_t maxValue =
                            static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                        castNode.widthHint = width > maxValue
                                                 ? std::numeric_limits<int32_t>::max()
                                                 : static_cast<int32_t>(width);
                    }
                }
                return addNodeForExpr(std::move(castNode));
            }
            return converted;
        }
        if (const auto* unary = expr.as_if<slang::ast::UnaryExpression>())
        {
            if (unary->op == slang::ast::UnaryOperator::Plus ||
                unary->op == slang::ast::UnaryOperator::Minus)
            {
                ExprNodeId operand = lowerExpression(unary->operand());
                if (operand == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                bool operandSigned = false;
                if (operand < lowering.values.size())
                {
                    operandSigned = lowering.values[operand].isSigned;
                }
                auto operandWidth = [&](ExprNodeId id) -> int32_t {
                    if (id == kInvalidPlanIndex || id >= lowering.values.size())
                    {
                        return 0;
                    }
                    return lowering.values[id].widthHint;
                };
                int32_t targetWidth = 0;
                if (widthContext > 0)
                {
                    targetWidth = widthContext;
                }
                else
                {
                    targetWidth = operandWidth(operand);
                }
                if (targetWidth == 0)
                {
                    uint64_t width = computeExprWidth(expr);
                    if (width > 0)
                    {
                        const uint64_t maxValue =
                            static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                        targetWidth = width > maxValue
                                          ? std::numeric_limits<int32_t>::max()
                                          : static_cast<int32_t>(width);
                    }
                }
                if (targetWidth > 0)
                {
                    const bool signExtend =
                        expr.type ? expr.type->isSigned() : operandSigned;
                    operand = resizeValueToWidth(operand, targetWidth, signExtend,
                                                 expr.sourceRange.start());
                    if (operand == kInvalidPlanIndex)
                    {
                        return kInvalidPlanIndex;
                    }
                }
                if (unary->op == slang::ast::UnaryOperator::Plus)
                {
                    if (cacheable)
                    {
                        lowered.emplace(&expr, operand);
                    }
                    return operand;
                }
                const bool signExtend =
                    expr.type ? expr.type->isSigned() : operandSigned;
                ExprNode zeroNode;
                zeroNode.kind = ExprNodeKind::Constant;
                zeroNode.literal = "0";
                zeroNode.location = expr.sourceRange.start();
                zeroNode.isSigned = signExtend;
                if (targetWidth > 0)
                {
                    zeroNode.widthHint = targetWidth;
                }
                ExprNodeId zeroId = addNode(nullptr, std::move(zeroNode));
                ExprNode negNode;
                negNode.kind = ExprNodeKind::Operation;
                negNode.op = wolvrix::lib::grh::OperationKind::kSub;
                negNode.operands = {zeroId, operand};
                negNode.location = expr.sourceRange.start();
                negNode.tempSymbol = makeTempSymbol();
                negNode.isSigned = signExtend;
                if (targetWidth > 0)
                {
                    negNode.widthHint = targetWidth;
                }
                if (negNode.widthHint == 0)
                {
                    applyExprWidthHint(negNode);
                }
                return addNodeForExpr(std::move(negNode));
            }
            const auto opKind = mapUnaryOp(unary->op);
            if (!opKind)
            {
                reportUnsupported(expr, "Unsupported unary operator");
                return kInvalidPlanIndex;
            }
            ExprNodeId operand = lowerExpression(unary->operand());
            if (operand == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            node.kind = ExprNodeKind::Operation;
            node.op = *opKind;
            node.operands = {operand};
            if (*opKind == wolvrix::lib::grh::OperationKind::kReduceAnd ||
                *opKind == wolvrix::lib::grh::OperationKind::kReduceOr ||
                *opKind == wolvrix::lib::grh::OperationKind::kReduceXor ||
                *opKind == wolvrix::lib::grh::OperationKind::kReduceNand ||
                *opKind == wolvrix::lib::grh::OperationKind::kReduceNor ||
                *opKind == wolvrix::lib::grh::OperationKind::kReduceXnor)
            {
                node.widthHint = 1;
            }
            if (node.widthHint == 0)
            {
                applyExprWidthHint(node);
            }
            node.tempSymbol = makeTempSymbol();
            return addNodeForExpr(std::move(node));
        }
        if (const auto* binary = expr.as_if<slang::ast::BinaryExpression>())
        {
            const auto opKind = mapBinaryOp(binary->op);
            if (!opKind)
            {
                reportUnsupported(expr, "Unsupported binary operator");
                return kInvalidPlanIndex;
            }
            const bool isShift =
                (*opKind == wolvrix::lib::grh::OperationKind::kShl ||
                 *opKind == wolvrix::lib::grh::OperationKind::kLShr ||
                 *opKind == wolvrix::lib::grh::OperationKind::kAShr);
            if (isShift)
            {
                ExprNodeId lhs = kInvalidPlanIndex;
                ExprNodeId rhs = kInvalidPlanIndex;
                {
                    WidthContextScope widthScope(*this, 0);
                    lhs = lowerExpression(binary->left());
                    rhs = lowerExpression(binary->right());
                }
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                node.kind = ExprNodeKind::Operation;
                node.op = *opKind;
                node.operands = {lhs, rhs};
                applyExprWidthHint(node);
                node.tempSymbol = makeTempSymbol();
                ExprNodeId shifted = addNodeForExpr(std::move(node));
                if (widthContext > 0 && shifted != kInvalidPlanIndex)
                {
                    const bool signExtend = expr.type ? expr.type->isSigned() : false;
                    return resizeValueToWidth(shifted, widthContext, signExtend,
                                              expr.sourceRange.start());
                }
                return shifted;
            }
            ExprNodeId lhs = lowerExpression(binary->left());
            ExprNodeId rhs = lowerExpression(binary->right());
            if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            const bool isArithmetic =
                (*opKind == wolvrix::lib::grh::OperationKind::kAdd ||
                 *opKind == wolvrix::lib::grh::OperationKind::kSub ||
                 *opKind == wolvrix::lib::grh::OperationKind::kMul ||
                 *opKind == wolvrix::lib::grh::OperationKind::kDiv ||
                 *opKind == wolvrix::lib::grh::OperationKind::kMod);
            const bool isBitwise =
                (*opKind == wolvrix::lib::grh::OperationKind::kAnd ||
                 *opKind == wolvrix::lib::grh::OperationKind::kOr ||
                 *opKind == wolvrix::lib::grh::OperationKind::kXor ||
                 *opKind == wolvrix::lib::grh::OperationKind::kXnor);
            auto operandWidth = [&](ExprNodeId id) -> int32_t {
                if (id == kInvalidPlanIndex || id >= lowering.values.size())
                {
                    return 0;
                }
                return lowering.values[id].widthHint;
            };
            const int32_t lhsWidth = operandWidth(lhs);
            const int32_t rhsWidth = operandWidth(rhs);
            int32_t targetWidth = 0;
            if (isArithmetic)
            {
                if (widthContext > 0)
                {
                    targetWidth = widthContext;
                }
                if (targetWidth == 0 && lhsWidth > 0 && rhsWidth > 0)
                {
                    if (*opKind == wolvrix::lib::grh::OperationKind::kMul)
                    {
                        const int64_t combined =
                            static_cast<int64_t>(lhsWidth) + static_cast<int64_t>(rhsWidth);
                        const int64_t maxValue =
                            static_cast<int64_t>(std::numeric_limits<int32_t>::max());
                        targetWidth = combined > maxValue
                                          ? std::numeric_limits<int32_t>::max()
                                          : static_cast<int32_t>(combined);
                    }
                    else
                    {
                        targetWidth = std::max(lhsWidth, rhsWidth);
                    }
                }
            }
            else if (isBitwise)
            {
                const bool lhsKnown = lhsWidth > 0;
                const bool rhsKnown = rhsWidth > 0;
                if (lhsKnown && rhsKnown)
                {
                    targetWidth = std::max(lhsWidth, rhsWidth);
                }
                else if (lhsKnown || rhsKnown)
                {
                    targetWidth = std::max(lhsWidth, rhsWidth);
                }
                else if (widthContext > 0)
                {
                    targetWidth = widthContext;
                }
            }
            if ((isArithmetic || isBitwise) && targetWidth > 0)
            {
                const bool signExtendLeft =
                    isBitwise
                        ? (binary->left().type ? binary->left().type->isSigned() : false)
                        : (expr.type ? expr.type->isSigned() : false);
                const bool signExtendRight =
                    isBitwise
                        ? (binary->right().type ? binary->right().type->isSigned() : false)
                        : (expr.type ? expr.type->isSigned() : false);
                lhs = resizeValueToWidth(lhs, targetWidth, signExtendLeft,
                                         expr.sourceRange.start());
                rhs = resizeValueToWidth(rhs, targetWidth, signExtendRight,
                                         expr.sourceRange.start());
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
            }
            node.kind = ExprNodeKind::Operation;
            node.op = *opKind;
            node.operands = {lhs, rhs};
            if ((isArithmetic || isBitwise) && targetWidth > 0)
            {
                node.widthHint = targetWidth;
            }
            if (node.widthHint == 0)
            {
                applyExprWidthHint(node);
            }
            node.tempSymbol = makeTempSymbol();
            return addNodeForExpr(std::move(node));
        }
        if (const auto* cond = expr.as_if<slang::ast::ConditionalExpression>())
        {
            if (cond->conditions.empty())
            {
                reportUnsupported(expr, "Conditional expression missing condition");
                return kInvalidPlanIndex;
            }
            if (cond->conditions.size() > 1)
            {
                reportUnsupported(expr, "Conditional expression with patterns unsupported");
            }
            const slang::ast::Expression& condExpr = *cond->conditions.front().expr;
            ExprNodeId condId = lowerExpression(condExpr);
            if (condId != kInvalidPlanIndex && condId < lowering.values.size())
            {
                const int32_t condWidth = lowering.values[condId].widthHint;
                if (condWidth > 1)
                {
                    ExprNode logicNode;
                    logicNode.kind = ExprNodeKind::Operation;
                    logicNode.op = wolvrix::lib::grh::OperationKind::kReduceOr;
                    logicNode.operands = {condId};
                    logicNode.location = condExpr.sourceRange.start();
                    logicNode.tempSymbol = makeTempSymbol();
                    logicNode.widthHint = 1;
                    condId = addNode(nullptr, std::move(logicNode));
                }
            }
            ExprNodeId lhs = kInvalidPlanIndex;
            ExprNodeId rhs = kInvalidPlanIndex;
            {
                WidthContextScope widthScope(*this, 0);
                lhs = lowerExpression(cond->left());
                rhs = lowerExpression(cond->right());
                if (condId == kInvalidPlanIndex || lhs == kInvalidPlanIndex ||
                    rhs == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
            }
            // Preserve natural width of MUX branches, but resize result if needed
            // This fixes issues like: (cond ? a : b) < 8 where a and b should not be truncated
            // Optimization: if both branches are constants and we have a width context,
            // resize the constants directly to avoid WIDTHTRUNC warnings
            if (widthContext > 0 && lhs != kInvalidPlanIndex && rhs != kInvalidPlanIndex)
            {
                const bool lhsIsConst =
                    lhs < lowering.values.size() &&
                    lowering.values[lhs].kind == ExprNodeKind::Constant;
                const bool rhsIsConst =
                    rhs < lowering.values.size() &&
                    lowering.values[rhs].kind == ExprNodeKind::Constant;
                if (lhsIsConst && rhsIsConst)
                {
                    const bool signExtend = expr.type ? expr.type->isSigned() : false;
                    lhs = resizeValueToWidth(lhs, widthContext, signExtend,
                                             expr.sourceRange.start());
                    rhs = resizeValueToWidth(rhs, widthContext, signExtend,
                                             expr.sourceRange.start());
                }
            }
            node.kind = ExprNodeKind::Operation;
            node.op = wolvrix::lib::grh::OperationKind::kMux;
            node.operands = {condId, lhs, rhs};
            node.tempSymbol = makeTempSymbol();
            ExprNodeId muxResult = addNodeForExpr(std::move(node));
            if (widthContext > 0 && muxResult != kInvalidPlanIndex)
            {
                const bool signExtend = expr.type ? expr.type->isSigned() : false;
                return resizeValueToWidth(muxResult, widthContext, signExtend,
                                          expr.sourceRange.start());
            }
            return muxResult;
        }
        if (const auto* concat = expr.as_if<slang::ast::ConcatenationExpression>())
        {
            std::vector<ExprNodeId> operands;
            operands.reserve(concat->operands().size());
            {
                WidthContextScope widthScope(*this, 0);
                for (const slang::ast::Expression* operand : concat->operands())
                {
                    if (!operand)
                    {
                        continue;
                    }
                    ExprNodeId id = lowerExpression(*operand);
                    if (id == kInvalidPlanIndex)
                    {
                        return kInvalidPlanIndex;
                    }
                    operands.push_back(id);
                }
            }
            node.kind = ExprNodeKind::Operation;
            node.op = wolvrix::lib::grh::OperationKind::kConcat;
            node.operands = std::move(operands);
            node.tempSymbol = makeTempSymbol();
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* pattern = expr.as_if<slang::ast::SimpleAssignmentPatternExpression>())
        {
            std::vector<ExprNodeId> operands;
            operands.reserve(pattern->elements().size());
            {
                WidthContextScope widthScope(*this, 0);
                for (const slang::ast::Expression* element : pattern->elements())
                {
                    if (!element)
                    {
                        continue;
                    }
                    ExprNodeId id = lowerExpression(*element);
                    if (id == kInvalidPlanIndex)
                    {
                        return kInvalidPlanIndex;
                    }
                    operands.push_back(id);
                }
            }
            node.kind = ExprNodeKind::Operation;
            node.op = wolvrix::lib::grh::OperationKind::kConcat;
            node.operands = std::move(operands);
            node.tempSymbol = makeTempSymbol();
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* repl = expr.as_if<slang::ast::ReplicationExpression>())
        {
            ExprNodeId count = kInvalidPlanIndex;
            ExprNodeId concat = kInvalidPlanIndex;
            {
                WidthContextScope widthScope(*this, 0);
                count = lowerExpression(repl->count());
                concat = lowerExpression(repl->concat());
            }
            if (count == kInvalidPlanIndex || concat == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            node.kind = ExprNodeKind::Operation;
            node.op = wolvrix::lib::grh::OperationKind::kReplicate;
            node.operands = {count, concat};
            node.tempSymbol = makeTempSymbol();
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* select = expr.as_if<slang::ast::ElementSelectExpression>())
        {
            if (auto chain = collectNetArraySelectChain(expr))
            {
                ExprNodeId bitIndex = kInvalidPlanIndex;
                int64_t sliceWidth = 0;
                if (buildNetArraySlice(*chain, bitIndex, sliceWidth,
                                       select->sourceRange.start()))
                {
                    ExprNodeId base = lowerExpression(*chain->baseExpr);
                    if (base != kInvalidPlanIndex && bitIndex != kInvalidPlanIndex)
                    {
                        const int64_t maxWidth =
                            static_cast<int64_t>(std::numeric_limits<int32_t>::max());
                        const int32_t width =
                            static_cast<int32_t>(std::min<int64_t>(sliceWidth, maxWidth));
                        return makeSliceDynamic(base, bitIndex, width,
                                                select->sourceRange.start());
                    }
                }
            }
            ExprNodeId value = lowerExpression(select->value());
            ExprNodeId index = kInvalidPlanIndex;
            auto clampWidth = [](uint64_t width) -> int32_t {
                if (width == 0)
                {
                    return 0;
                }
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                return width > maxValue ? std::numeric_limits<int32_t>::max()
                                        : static_cast<int32_t>(width);
            };
            int32_t indexWidth = clampWidth(computeExprWidth(select->selector()));
            if (indexWidth <= 0)
            {
                indexWidth = 32;
            }
            {
                WidthContextScope widthScope(*this, indexWidth);
                index = lowerExpression(select->selector());
            }
            if (value == kInvalidPlanIndex || index == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            if (auto elementWidth = getPackedArrayElementWidth(select->value().type))
            {
                ExprNodeId laneIndex =
                    adjustPackedElementLaneIndex(select->value().type, index,
                                                 select->sourceRange.start());
                return makeSliceArray(value, laneIndex, *elementWidth,
                                      select->sourceRange.start());
            }
            index = adjustPackedIndex(select->value().type, index,
                                      select->sourceRange.start());
            node.kind = ExprNodeKind::Operation;
            node.op = wolvrix::lib::grh::OperationKind::kSliceDynamic;
            node.operands = {value, index};
            node.tempSymbol = makeTempSymbol();
            applyExprWidthHint(node);
            return addNodeForExpr(std::move(node));
        }
        if (const auto* range = expr.as_if<slang::ast::RangeSelectExpression>())
        {
            ExprNodeId value = lowerExpression(range->value());
            ExprNodeId left = kInvalidPlanIndex;
            ExprNodeId right = kInvalidPlanIndex;
            auto clampWidth = [](uint64_t width) -> int32_t {
                if (width == 0)
                {
                    return 0;
                }
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                return width > maxValue ? std::numeric_limits<int32_t>::max()
                                        : static_cast<int32_t>(width);
            };
            int32_t indexWidth = clampWidth(computeExprWidth(range->left()));
            if (indexWidth <= 0)
            {
                indexWidth = clampWidth(computeExprWidth(range->right()));
            }
            if (indexWidth <= 0)
            {
                indexWidth = 32;
            }
            {
                WidthContextScope widthScope(*this, indexWidth);
                left = lowerExpression(range->left());
                right = lowerExpression(range->right());
            }
            if (value == kInvalidPlanIndex || left == kInvalidPlanIndex ||
                right == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            int32_t sliceWidth = clampWidth(computeExprWidth(*range));
            if (sliceWidth <= 0)
            {
                sliceWidth = 1;
            }
            auto addConst = [&](int64_t literal) -> ExprNodeId {
                ExprNode constNode;
                constNode.kind = ExprNodeKind::Constant;
                constNode.literal = std::to_string(literal);
                constNode.location = range->sourceRange.start();
                constNode.widthHint = indexWidth;
                return addNode(nullptr, std::move(constNode));
            };
            auto addSub = [&](ExprNodeId lhs, ExprNodeId rhs) -> ExprNodeId {
                ExprNode subNode;
                subNode.kind = ExprNodeKind::Operation;
                subNode.op = wolvrix::lib::grh::OperationKind::kSub;
                subNode.operands = {lhs, rhs};
                subNode.location = range->sourceRange.start();
                subNode.tempSymbol = makeTempSymbol();
                subNode.widthHint = indexWidth;
                return addNode(nullptr, std::move(subNode));
            };
            auto addAdd = [&](ExprNodeId lhs, ExprNodeId rhs) -> ExprNodeId {
                ExprNode addNodeValue;
                addNodeValue.kind = ExprNodeKind::Operation;
                addNodeValue.op = wolvrix::lib::grh::OperationKind::kAdd;
                addNodeValue.operands = {lhs, rhs};
                addNodeValue.location = range->sourceRange.start();
                addNodeValue.tempSymbol = makeTempSymbol();
                addNodeValue.widthHint = indexWidth;
                return addNode(nullptr, std::move(addNodeValue));
            };
            ExprNodeId indexExpr = kInvalidPlanIndex;
            switch (range->getSelectionKind())
            {
            case slang::ast::RangeSelectionKind::Simple: {
                auto leftConst = evalConstInt(plan, lowering, left);
                auto rightConst = evalConstInt(plan, lowering, right);
                if (!leftConst || !rightConst)
                {
                    reportUnsupported(expr, "Dynamic range select is unsupported");
                    return kInvalidPlanIndex;
                }
                const int64_t low = std::min(*leftConst, *rightConst);
                indexExpr = addConst(low);
                break;
            }
            case slang::ast::RangeSelectionKind::IndexedUp:
                indexExpr = left;
                break;
            case slang::ast::RangeSelectionKind::IndexedDown: {
                auto widthConst = evalConstInt(plan, lowering, right);
                if (widthConst)
                {
                    if (*widthConst <= 0)
                    {
                        reportUnsupported(expr, "Indexed part-select width must be positive");
                        return kInvalidPlanIndex;
                    }
                    if (*widthConst == 1)
                    {
                        indexExpr = left;
                    }
                    else
                    {
                        ExprNodeId offset = addConst(*widthConst - 1);
                        indexExpr = addSub(left, offset);
                    }
                }
                else
                {
                    ExprNodeId one = addConst(1);
                    ExprNodeId widthMinus = addSub(right, one);
                    indexExpr = addSub(left, widthMinus);
                }
                break;
            }
            default:
                reportUnsupported(expr, "Unsupported range select kind");
                return kInvalidPlanIndex;
            }
            if (indexExpr == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            if (auto packedElement = getPackedArrayElementSlice(range->value().type))
            {
                const auto& canonical = range->value().type->getCanonicalType();
                const auto& packed = canonical.as<slang::ast::PackedArrayType>();
                const bool ascending = packed.range.left <= packed.range.right;
                ExprNodeId elementStart = indexExpr;

                if (range->getSelectionKind() == slang::ast::RangeSelectionKind::Simple)
                {
                    auto leftConst = evalConstInt(plan, lowering, left);
                    auto rightConst = evalConstInt(plan, lowering, right);
                    if (!leftConst || !rightConst)
                    {
                        reportUnsupported(expr, "Dynamic range select is unsupported");
                        return kInvalidPlanIndex;
                    }
                    const int64_t low = std::min<int64_t>(packed.range.left, packed.range.right);
                    const int64_t high = std::max<int64_t>(packed.range.left, packed.range.right);
                    auto laneOf = [&](int64_t svIndex) -> int64_t {
                        return ascending ? high - svIndex : svIndex - low;
                    };
                    const int64_t startLane = std::min(laneOf(*leftConst), laneOf(*rightConst));
                    indexExpr = addConst(startLane * packedElement->second);
                }
                else
                {
                    auto widthConst = evalConstInt(plan, lowering, right);
                    if (widthConst && *widthConst > 1)
                    {
                        const bool needUpperEndpoint =
                            (range->getSelectionKind() == slang::ast::RangeSelectionKind::IndexedUp &&
                             ascending) ||
                            (range->getSelectionKind() == slang::ast::RangeSelectionKind::IndexedDown &&
                             !ascending);
                        if (needUpperEndpoint)
                        {
                            ExprNodeId offset = addConst(*widthConst - 1);
                            if (range->getSelectionKind() ==
                                slang::ast::RangeSelectionKind::IndexedUp)
                            {
                                elementStart = addAdd(left, offset);
                            }
                            else
                            {
                                elementStart = addSub(left, offset);
                            }
                        }
                    }
                    elementStart = adjustPackedElementLaneIndex(
                        range->value().type, elementStart, range->sourceRange.start());
                    indexExpr = scalePackedElementLaneIndex(
                        elementStart, packedElement->second, range->sourceRange.start());
                }
            }
            else
            {
                indexExpr =
                    adjustPackedIndex(range->value().type, indexExpr,
                                      range->sourceRange.start());
            }
            if (indexExpr == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            node.kind = ExprNodeKind::Operation;
            node.op = wolvrix::lib::grh::OperationKind::kSliceDynamic;
            node.operands = {value, indexExpr};
            node.tempSymbol = makeTempSymbol();
            node.widthHint = sliceWidth;
            return addNodeForExpr(std::move(node));
        }

        reportUnsupported(expr, "Unsupported expression kind");
        return kInvalidPlanIndex;
    }

    WriteRangeKind mapRangeKind(slang::ast::RangeSelectionKind kind) const
    {
        switch (kind)
        {
        case slang::ast::RangeSelectionKind::Simple:
            return WriteRangeKind::Simple;
        case slang::ast::RangeSelectionKind::IndexedUp:
            return WriteRangeKind::IndexedUp;
        case slang::ast::RangeSelectionKind::IndexedDown:
            return WriteRangeKind::IndexedDown;
        default:
            break;
        }
        return WriteRangeKind::Simple;
    }

    uint64_t computeExprWidth(const slang::ast::Expression& expr)
    {
        if (!expr.type)
        {
            return 0;
        }
        return computeFixedWidth(*expr.type, *plan.body, diagnostics);
    }

    ExprNodeId makeRhsSlice(ExprNodeId value, uint64_t high, uint64_t low,
                            slang::SourceLocation location)
    {
        if (value == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        const uint64_t lo = std::min(high, low);
        const uint64_t hi = std::max(high, low);
        const uint64_t span = hi - lo + 1;
        const uint64_t maxValue =
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
        const int32_t width =
            span > maxValue ? std::numeric_limits<int32_t>::max()
                            : static_cast<int32_t>(span);
        int32_t indexWidth = 0;
        if (value < lowering.values.size())
        {
            indexWidth = lowering.values[value].widthHint;
        }
        if (indexWidth <= 0)
        {
            indexWidth = 32;
        }
        ExprNode constNode;
        constNode.kind = ExprNodeKind::Constant;
        constNode.literal = std::to_string(lo);
        constNode.location = location;
        constNode.widthHint = indexWidth;
        ExprNodeId index = addNode(nullptr, std::move(constNode));
        return makeSliceDynamic(value, index, width, location);
    }

    bool resolveLValueTargets(const slang::ast::Expression& expr,
                              std::vector<LValueTarget>& targets,
                              LValueCompositeInfo& composite)
    {
        if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
        {
            if (conversion->isImplicit())
            {
                return resolveLValueTargets(conversion->operand(), targets, composite);
            }
            reportUnsupported(expr, "Unsupported explicit conversion in LHS");
            return false;
        }
        if (const auto* concat = expr.as_if<slang::ast::ConcatenationExpression>())
        {
            composite.isComposite = true;
            for (const slang::ast::Expression* operand : concat->operands())
            {
                if (!operand)
                {
                    continue;
                }
                if (!resolveLValueTargets(*operand, targets, composite))
                {
                    return false;
                }
            }
            return true;
        }
        if (const auto* stream = expr.as_if<slang::ast::StreamingConcatenationExpression>())
        {
            composite.isComposite = true;
            if (stream->getSliceSize() != 0)
            {
                reportUnsupported(expr, "Right-to-left streaming LHS is unsupported");
                return false;
            }
            for (const auto& element : stream->streams())
            {
                if (element.withExpr)
                {
                    reportUnsupported(*element.withExpr,
                                      "Streaming LHS with with-clause is unsupported");
                    return false;
                }
                if (!resolveLValueTargets(*element.operand, targets, composite))
                {
                    return false;
                }
            }
            return true;
        }

        LValueTarget target;
        std::string xmrPath;
        target.target = resolveLValueSymbol(expr, target.slices, &xmrPath);
        if (!xmrPath.empty())
        {
            target.isXmr = true;
            target.xmrPath = std::move(xmrPath);
        }
        if (!target.target.valid() && !target.isXmr)
        {
            return false;
        }
        target.width = computeExprWidth(expr);
        if (composite.isComposite && target.width == 0)
        {
            reportUnsupported(expr, "Unsupported LHS width in assignment");
            return false;
        }
        target.location = expr.sourceRange.start();
        targets.push_back(std::move(target));
        return true;
    }

    PlanSymbolId resolveLValueSymbol(const slang::ast::Expression& expr,
                                     std::vector<WriteSlice>& slices,
                                     std::string* xmrPath)
    {
        if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
        {
            if (auto local = lookupProceduralLocal(named->symbol))
            {
                return *local;
            }
            return lookupPlanSignal(plan, named->symbol);
        }
        if (const auto* hier = expr.as_if<slang::ast::HierarchicalValueExpression>())
        {
            if (xmrPath)
            {
                if (auto path = extractXmrPath(*hier))
                {
                    *xmrPath = std::move(*path);
                }
            }
            return {};
        }
        if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
        {
            if (conversion->isImplicit())
            {
                return resolveLValueSymbol(conversion->operand(), slices, xmrPath);
            }
            return {};
        }
        if (const auto* member = expr.as_if<slang::ast::MemberAccessExpression>())
        {
            PlanSymbolId base = resolveLValueSymbol(member->value(), slices, xmrPath);
            if (!base.valid())
            {
                return {};
            }
            if (member->member.name.empty())
            {
                return {};
            }
            WriteSlice slice;
            slice.kind = WriteSliceKind::MemberSelect;
            slice.member = plan.symbolTable.intern(member->member.name);
            slice.location = member->sourceRange.start();
            slices.push_back(std::move(slice));
            return base;
        }
        if (const auto* select = expr.as_if<slang::ast::ElementSelectExpression>())
        {
            if (auto chain = collectNetArraySelectChain(expr))
            {
                ExprNodeId bitIndex = kInvalidPlanIndex;
                int64_t sliceWidth = 0;
                if (buildNetArraySlice(*chain, bitIndex, sliceWidth,
                                       select->sourceRange.start()))
                {
                    WriteSlice slice;
                    slice.kind = WriteSliceKind::RangeSelect;
                    slice.rangeKind = WriteRangeKind::IndexedUp;
                    slice.left = bitIndex;
                    slice.right =
                        addConstantLiteral(std::to_string(sliceWidth),
                                           select->sourceRange.start());
                    slice.location = select->sourceRange.start();
                    slices.push_back(std::move(slice));
                    return lookupPlanSignal(plan, chain->baseExpr->symbol);
                }
            }
            PlanSymbolId base = resolveLValueSymbol(select->value(), slices, xmrPath);
            if (!base.valid())
            {
                return {};
            }
            ExprNodeId index = kInvalidPlanIndex;
            {
                WidthContextScope widthScope(*this, 0);
                index = lowerExpression(select->selector());
            }
            if (index == kInvalidPlanIndex)
            {
                return {};
            }
            WriteSlice slice;
            slice.kind = WriteSliceKind::BitSelect;
            slice.index = index;
            slice.location = select->sourceRange.start();
            slices.push_back(std::move(slice));
            return base;
        }
        if (const auto* range = expr.as_if<slang::ast::RangeSelectExpression>())
        {
            PlanSymbolId base = resolveLValueSymbol(range->value(), slices, xmrPath);
            if (!base.valid())
            {
                return {};
            }
            ExprNodeId left = kInvalidPlanIndex;
            ExprNodeId right = kInvalidPlanIndex;
            {
                WidthContextScope widthScope(*this, 0);
                left = lowerExpression(range->left());
                right = lowerExpression(range->right());
            }
            if (left == kInvalidPlanIndex || right == kInvalidPlanIndex)
            {
                return {};
            }
            WriteSlice slice;
            slice.kind = WriteSliceKind::RangeSelect;
            slice.rangeKind = mapRangeKind(range->getSelectionKind());
            slice.left = left;
            slice.right = right;
            slice.location = range->sourceRange.start();
            slices.push_back(std::move(slice));
            return base;
        }
        return {};
    }

    void reportUnsupported(const slang::ast::Expression& expr, std::string_view message)
    {
        if (diagnostics)
        {
            diagnostics->error(expr.sourceRange.start(), std::string(message));
        }
    }

    void reportUnsupported(const slang::ast::Statement& stmt, std::string_view message)
    {
        if (diagnostics)
        {
            diagnostics->error(stmt.sourceRange.start(), std::string(message));
        }
    }

    void reportError(const slang::ast::Statement& stmt, std::string_view message)
    {
        if (diagnostics)
        {
            diagnostics->error(stmt.sourceRange.start(), std::string(message));
        }
    }

    void reportError(const slang::ast::Expression& expr, std::string_view message)
    {
        if (diagnostics)
        {
            diagnostics->error(expr.sourceRange.start(), std::string(message));
        }
    }

    void reportLoopControlError(const slang::ast::Statement& stmt, std::string_view header)
    {
        std::string message(header);
        if (loopControlFailure)
        {
            message.append(": ");
            message.append(*loopControlFailure);
        }
        reportError(stmt, message);
    }

    void reportLoopFailure(const slang::ast::Statement& stmt, std::string_view header)
    {
        std::string message(header);
        if (loopControlFailure)
        {
            message.append(": ");
            message.append(*loopControlFailure);
        }
        reportError(stmt, message);
    }

    void clearLoopControlFailure()
    {
        loopControlFailure.reset();
    }

    void setLoopControlFailure(std::string message)
    {
        if (!loopControlFailure)
        {
            loopControlFailure = std::move(message);
        }
    }

    // Try to extract init value from expression for initial block memory assignment
    // Returns non-empty string if it's a simple pattern we can capture
    std::string tryExtractInitValue(const slang::ast::Expression& rhs)
    {
        // Case 0: Conversion expression - unwrap and retry
        if (rhs.kind == slang::ast::ExpressionKind::Conversion) {
            const auto& conv = rhs.as<slang::ast::ConversionExpression>();
            return tryExtractInitValue(conv.operand());
        }

        // Case 1: IntegerLiteral -> hex string representation
        if (rhs.kind == slang::ast::ExpressionKind::IntegerLiteral) {
            const auto& lit = rhs.as<slang::ast::IntegerLiteral>();
            auto value = lit.getValue();
            std::ostringstream oss;
            // Output as hex with appropriate width (at least 1 hex digit)
            oss << value.getBitWidth() << "'h" << std::hex << std::setfill('0');
            int hexDigits = (value.getBitWidth() + 3) / 4;
            if (hexDigits < 1) hexDigits = 1;
            oss << std::setw(hexDigits) << value.getRawPtr()[0];
            return oss.str();
        }

        // Case 2: UnbasedUnsizedIntegerLiteral -> "0" or "1"
        if (rhs.kind == slang::ast::ExpressionKind::UnbasedUnsizedIntegerLiteral) {
            const auto& lit = rhs.as<slang::ast::UnbasedUnsizedIntegerLiteral>();
            auto value = lit.getValue();
            // Check if it's 0 or 1
            if (value.getRawPtr()[0] == 0) {
                return "0";
            } else if (value.getRawPtr()[0] == 1) {
                return "1";
            }
            // Other unsized values -> treat as complex
            return "";
        }

        // Case 3: $random or $urandom call
        if (rhs.kind == slang::ast::ExpressionKind::Call) {
            const auto& call = rhs.as<slang::ast::CallExpression>();
            const std::string name(call.getSubroutineName());
            if (name == "$random" || name == "$urandom") {
                const auto& args = call.arguments();
                if (args.empty()) {
                    return "$random";
                } else if (args.size() == 1) {
                    // Try to extract seed if it's a constant
                    if (args[0]->kind == slang::ast::ExpressionKind::IntegerLiteral) {
                        const auto& seedLit = args[0]->as<slang::ast::IntegerLiteral>();
                        auto seed = seedLit.getValue();
                        std::ostringstream oss;
                        oss << "$random(" << seed.getRawPtr()[0] << ")";
                        return oss.str();
                    }
                }
            }
        }

        // Complex expression -> can't capture as initValue
        return "";
    }

    // Try to record memory initialization in initial block
    // Returns true if this assignment was handled as a memory init
    bool tryRecordMemoryInit(const slang::ast::AssignmentExpression& expr,
                             const std::vector<LValueTarget>& targets)
    {
        // Only handle simple assignments to indexed memory elements in initial blocks
        if (eventContext.procKind != ProcKind::Initial) {
            return false;
        }

        // Must have exactly one target
        if (targets.size() != 1) {
            return false;
        }

        const auto& target = targets[0];

        // Check if this is an indexed element (memory write)
        // Memory access is represented as BitSelect or RangeSelect on a symbol
        bool isIndexedAccess = false;
        for (const auto& slice : target.slices) {
            if (slice.kind == WriteSliceKind::BitSelect || 
                slice.kind == WriteSliceKind::RangeSelect) {
                isIndexedAccess = true;
                break;
            }
        }
        if (!isIndexedAccess) {
            return false;
        }

        // Get the memory symbol
        PlanSymbolId memorySym = target.target;

        // Try to extract init value from RHS
        std::string initValue = tryExtractInitValue(expr.right());
        if (initValue.empty()) {
            // Complex expression - will be ignored by emitter with warning
            return false;
        }

        // Record the memory init
        MemoryInit init;
        init.memory = memorySym;
        init.kind = "literal";
        init.initValue = std::move(initValue);
        init.start = -1;  // Unknown index (loop variable)
        init.len = 0;
        init.location = expr.sourceRange.start();

        lowering.memoryInits.push_back(std::move(init));

        return true;
    }

    // Try to record register initialization in initial block
    // Returns true if this assignment was handled as a register init
    bool tryRecordRegisterInit(const slang::ast::AssignmentExpression& expr,
                               const std::vector<LValueTarget>& targets)
    {
        // Only handle simple assignments in initial blocks
        if (eventContext.procKind != ProcKind::Initial) {
            return false;
        }

        // Must have exactly one target
        if (targets.size() != 1) {
            return false;
        }

        const auto& target = targets[0];

        // Check this is NOT an indexed access (register, not memory)
        // Register access has no slices or only MemberSelect slices
        bool isIndexedAccess = false;
        for (const auto& slice : target.slices) {
            if (slice.kind == WriteSliceKind::BitSelect || 
                slice.kind == WriteSliceKind::RangeSelect) {
                isIndexedAccess = true;
                break;
            }
        }
        if (isIndexedAccess) {
            return false;
        }

        // Get the register symbol
        PlanSymbolId regSym = target.target;

        // Try to extract init value from RHS
        std::string initValue = tryExtractInitValue(expr.right());
        if (initValue.empty()) {
            // Complex expression - will be ignored by emitter with warning
            return false;
        }

        // Record the register init
        RegisterInit init;
        init.reg = regSym;
        init.initValue = std::move(initValue);
        init.location = expr.sourceRange.start();

        lowering.registerInits.push_back(std::move(init));

        return true;
    }

    std::optional<slang::ConstantValue> buildUnknownValue(const slang::ast::Type& type) const
    {
        if (type.isIntegral())
        {
            const uint64_t rawWidth = type.getBitstreamWidth();
            const slang::bitwidth_t width =
                static_cast<slang::bitwidth_t>(rawWidth > 0 ? rawWidth : 1);
            slang::SVInt init = slang::SVInt::createFillX(width, type.isSigned());
            return slang::ConstantValue(std::move(init));
        }
        const auto* elemType = type.getArrayElementType();
        if (!elemType)
        {
            return std::nullopt;
        }
        if (!type.hasFixedRange())
        {
            return std::nullopt;
        }
        const auto range = type.getFixedRange();
        const uint64_t count = range.fullWidth();
        if (count == 0)
        {
            return std::nullopt;
        }
        auto elemValue = buildUnknownValue(*elemType);
        if (!elemValue)
        {
            return std::nullopt;
        }
        slang::ConstantValue::Elements elements;
        elements.reserve(count);
        for (uint64_t i = 0; i < count; ++i)
        {
            elements.push_back(*elemValue);
        }
        return slang::ConstantValue(std::move(elements));
    }

    static std::optional<std::string> formatInitLiteral(const slang::ConstantValue& value)
    {
        if (value.isUnpacked())
        {
            std::vector<std::string> elements;
            for (const auto& elem : value.elements())
            {
                auto literal = formatInitLiteral(elem);
                if (!literal)
                {
                    return std::nullopt;
                }
                elements.push_back(*literal);
            }
            if (elements.empty())
            {
                return std::nullopt;
            }
            std::string out;
            out.push_back('{');
            for (std::size_t i = 0; i < elements.size(); ++i)
            {
                if (i != 0)
                {
                    out.append(", ");
                }
                out.append(elements[i]);
            }
            out.push_back('}');
            return out;
        }
        if (!value.isInteger())
        {
            return std::nullopt;
        }
        const auto& integer = value.integer();
        if (integer.hasUnknown())
        {
            return std::nullopt;
        }
        return formatIntegerLiteral(integer);
    }

    static std::optional<std::vector<std::string>>
    formatPackedAggregateInitRows(const SignalInfo& signal, const slang::ConstantValue& value)
    {
        if (signal.memoryRows <= 0)
        {
            return std::nullopt;
        }

        const std::size_t rows = static_cast<std::size_t>(signal.memoryRows);
        std::vector<std::string> literals;
        literals.reserve(rows);

        if (value.isUnpacked())
        {
            auto elems = value.elements();
            if (elems.size() != rows)
            {
                return std::nullopt;
            }
            for (const auto& elem : elems)
            {
                auto literal = formatInitLiteral(elem);
                if (!literal)
                {
                    return std::nullopt;
                }
                literals.push_back(*literal);
            }
            return literals;
        }

        if (!value.isInteger() || signal.width <= 0)
        {
            return std::nullopt;
        }
        const auto& integer = value.integer();
        if (integer.hasUnknown())
        {
            return std::nullopt;
        }

        const int32_t rowWidth = signal.width;
        const int64_t totalWidth = static_cast<int64_t>(rowWidth) * signal.memoryRows;
        if (totalWidth <= 0 ||
            totalWidth > static_cast<int64_t>(std::numeric_limits<int32_t>::max()))
        {
            return std::nullopt;
        }

        slang::SVInt sized = integer.resize(static_cast<slang::bitwidth_t>(totalWidth));
        for (int64_t row = 0; row < signal.memoryRows; ++row)
        {
            const int32_t lsb = static_cast<int32_t>(row * rowWidth);
            const int32_t msb = lsb + rowWidth - 1;
            literals.push_back(formatIntegerLiteral(sized.slice(msb, lsb)));
        }
        return literals;
    }

    static void appendLiteralMemoryInits(std::vector<MemoryInit>& memInits,
                                         PlanSymbolId memory,
                                         const std::vector<std::string>& literals,
                                         slang::SourceLocation location)
    {
        if (literals.empty())
        {
            return;
        }

        bool allSame = true;
        for (std::size_t i = 1; i < literals.size(); ++i)
        {
            if (literals[i] != literals[0])
            {
                allSame = false;
                break;
            }
        }

        if (allSame)
        {
            MemoryInit init;
            init.memory = memory;
            init.kind = "literal";
            init.initValue = literals[0];
            init.start = -1;
            init.len = 0;
            init.location = location;
            memInits.push_back(std::move(init));
            return;
        }

        for (std::size_t i = 0; i < literals.size(); ++i)
        {
            MemoryInit init;
            init.memory = memory;
            init.kind = "literal";
            init.initValue = literals[i];
            init.start = static_cast<int64_t>(i);
            init.len = 1;
            init.location = location;
            memInits.push_back(std::move(init));
        }
    }

public:
    bool tryEvaluateInitialBlock(const slang::ast::ProceduralBlockSymbol& block)
    {
        if (eventContext.procKind != ProcKind::Initial)
        {
            return false;
        }

        struct InitialSymbolCollector
            : public slang::ast::ASTVisitor<InitialSymbolCollector, true, true> {
            std::unordered_set<const slang::ast::ValueSymbol*> symbols;
            std::unordered_set<const slang::ast::ValueSymbol*> assignedSymbols;
            bool hasHier = false;

            void handle(const slang::ast::NamedValueExpression& expr)
            {
                if (expr.symbol.name.empty())
                {
                    return;
                }
                if (const auto* value = expr.symbol.as_if<slang::ast::ValueSymbol>())
                {
                    symbols.insert(value);
                }
            }

            void handle(const slang::ast::HierarchicalValueExpression&)
            {
                hasHier = true;
            }

            void collectAssignedSymbol(const slang::ast::Expression& expr)
            {
                if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
                {
                    if (named->symbol.name.empty())
                    {
                        return;
                    }
                    if (const auto* value = named->symbol.as_if<slang::ast::ValueSymbol>())
                    {
                        assignedSymbols.insert(value);
                    }
                    return;
                }
                if (const auto* hier = expr.as_if<slang::ast::HierarchicalValueExpression>())
                {
                    hasHier = true;
                    (void)hier;
                    return;
                }
                if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
                {
                    if (conversion->isImplicit())
                    {
                        collectAssignedSymbol(conversion->operand());
                    }
                    return;
                }
                if (const auto* concat = expr.as_if<slang::ast::ConcatenationExpression>())
                {
                    for (const slang::ast::Expression* operand : concat->operands())
                    {
                        if (operand)
                        {
                            collectAssignedSymbol(*operand);
                        }
                    }
                    return;
                }
                if (const auto* stream =
                        expr.as_if<slang::ast::StreamingConcatenationExpression>())
                {
                    for (const auto& element : stream->streams())
                    {
                        if (element.operand)
                        {
                            collectAssignedSymbol(*element.operand);
                        }
                    }
                    return;
                }
                if (const auto* member = expr.as_if<slang::ast::MemberAccessExpression>())
                {
                    collectAssignedSymbol(member->value());
                    return;
                }
                if (const auto* select = expr.as_if<slang::ast::ElementSelectExpression>())
                {
                    collectAssignedSymbol(select->value());
                    return;
                }
                if (const auto* range = expr.as_if<slang::ast::RangeSelectExpression>())
                {
                    collectAssignedSymbol(range->value());
                    return;
                }
            }

            void handle(const slang::ast::AssignmentExpression& expr)
            {
                collectAssignedSymbol(expr.left());
                visitDefault(expr);
            }
        };

        InitialSymbolCollector collector;
        block.getBody().visit(collector);
        if (collector.hasHier)
        {
            return false;
        }

        auto initEvalContext = [&]() -> std::optional<slang::ast::EvalContext> {
            slang::ast::EvalContext ctx(*plan.body, slang::ast::EvalFlags::IsScript);
            for (const slang::ast::ValueSymbol* symbol : collector.symbols)
            {
                if (!symbol || symbol->name.empty())
                {
                    continue;
                }
                if (symbol->kind == slang::ast::SymbolKind::Parameter ||
                    symbol->kind == slang::ast::SymbolKind::TypeParameter)
                {
                    continue;
                }
                PlanSymbolId id = lookupPlanSignal(plan, *symbol);
                if (!id.valid())
                {
                    continue;
                }
                auto value = buildUnknownValue(symbol->getType());
                if (!value)
                {
                    return std::nullopt;
                }
                ctx.createLocal(symbol, std::move(*value));
            }
            return ctx;
        };

        auto seedActiveHighReset = [&](slang::ast::EvalContext& ctx) {
            for (const slang::ast::ValueSymbol* symbol : collector.symbols)
            {
                if (!symbol || symbol->name != "reset")
                {
                    continue;
                }
                PlanSymbolId id = lookupPlanSignal(plan, *symbol);
                if (!id.valid())
                {
                    continue;
                }
                const PortInfo* port = findPortBySymbol(id);
                if (!port || port->direction != PortDirection::Input || port->width != 1 ||
                    !symbol->getType().isIntegral())
                {
                    continue;
                }
                ctx.createLocal(symbol,
                                slang::ConstantValue(slang::SVInt(1, UINT64_C(1), false)));
            }
        };

        auto collectInits = [&](slang::ast::EvalContext& ctx)
            -> std::optional<std::pair<std::vector<RegisterInit>, std::vector<MemoryInit>>> {
            using ER = slang::ast::Statement::EvalResult;
            ER evalResult = block.getBody().eval(ctx);
            if (evalResult != ER::Success && evalResult != ER::Return)
            {
                return std::nullopt;
            }

            std::vector<RegisterInit> regInits;
            std::vector<MemoryInit> memInits;
            for (const slang::ast::ValueSymbol* symbol : collector.assignedSymbols)
            {
                if (!symbol || symbol->name.empty())
                {
                    continue;
                }
                PlanSymbolId id = lookupPlanSignal(plan, *symbol);
                if (!id.valid())
                {
                    continue;
                }
                const SignalInfo* signal = findSignalBySymbol(plan, id);
                if (!signal)
                {
                    continue;
                }
                slang::ConstantValue* value = ctx.findLocal(symbol);
                if (!value)
                {
                    continue;
                }

                if ((signal->memoryRows > 0 || signal->kind == SignalKind::Memory) &&
                    !isPackedAggregateVariable(*signal))
                {
                    if (!value->isUnpacked())
                    {
                        return std::nullopt;
                    }
                    auto elems = value->elements();
                    if (signal->memoryRows > 0 &&
                        elems.size() != static_cast<std::size_t>(signal->memoryRows))
                    {
                        return std::nullopt;
                    }

                    std::vector<std::string> literals;
                    literals.reserve(elems.size());
                    for (const auto& elem : elems)
                    {
                        auto literal = formatInitLiteral(elem);
                        if (!literal)
                        {
                            return std::nullopt;
                        }
                        literals.push_back(*literal);
                    }
                    appendLiteralMemoryInits(memInits, id, literals, block.location);
                    continue;
                }

                if (isPackedAggregateVariable(*signal))
                {
                    auto literals = formatPackedAggregateInitRows(*signal, *value);
                    if (!literals)
                    {
                        return std::nullopt;
                    }
                    appendLiteralMemoryInits(memInits, id, *literals, block.location);
                    continue;
                }

                auto literal = formatInitLiteral(*value);
                if (!literal)
                {
                    return std::nullopt;
                }
                RegisterInit init;
                init.reg = id;
                init.initValue = *literal;
                init.location = block.location;
                regInits.push_back(std::move(init));
            }
            return std::make_pair(std::move(regInits), std::move(memInits));
        };

        auto ctx = initEvalContext();
        if (!ctx)
        {
            return false;
        }

        struct ReadmemCollector
            : public slang::ast::ASTVisitor<ReadmemCollector, true, true> {
            StmtLowererState& state;
            std::unordered_set<const slang::ast::CallExpression*> seen;
            explicit ReadmemCollector(StmtLowererState& state) : state(state) {}

            void handle(const slang::ast::ExpressionStatement& stmt)
            {
                const auto* call = stmt.expr.as_if<slang::ast::CallExpression>();
                if (!call || !call->isSystemCall())
                {
                    return;
                }
                if (!seen.insert(call).second)
                {
                    return;
                }
                const std::string name =
                    normalizeSystemTaskName(call->getSubroutineName());
                if (name == "readmemh" || name == "readmemb")
                {
                    state.emitReadmemCall(*call, name);
                }
            }
        };

        ReadmemCollector readmemCollector(*this);
        block.getBody().visit(readmemCollector);

        auto inits = collectInits(*ctx);
        if (!inits)
        {
            auto resetCtx = initEvalContext();
            if (!resetCtx)
            {
                return false;
            }
            seedActiveHighReset(*resetCtx);
            inits = collectInits(*resetCtx);
            if (!inits)
            {
                return false;
            }
        }

        auto& regInits = inits->first;
        auto& memInits = inits->second;
        if (!regInits.empty())
        {
            lowering.registerInits.insert(lowering.registerInits.end(),
                                          std::make_move_iterator(regInits.begin()),
                                          std::make_move_iterator(regInits.end()));
        }
        if (!memInits.empty())
        {
            lowering.memoryInits.insert(lowering.memoryInits.end(),
                                        std::make_move_iterator(memInits.begin()),
                                        std::make_move_iterator(memInits.end()));
        }

        return true;
    }

};

void lowerStmtGenerateBlock(const slang::ast::GenerateBlockSymbol& block,
                            StmtLowererState& state);
void lowerStmtGenerateBlockArray(const slang::ast::GenerateBlockArraySymbol& array,
                                 StmtLowererState& state);

void lowerStmtProceduralBlock(const slang::ast::ProceduralBlockSymbol& block,
                              StmtLowererState& state)
{
    const ControlDomain saved = state.domain;
    const StmtLowererState::EventContext savedEvent = state.eventContext;
    state.domain = classifyProceduralBlock(block);
    state.eventContext = state.buildEventContext(block);
    if (state.eventContext.procKind == ProcKind::Initial)
    {
        if (state.tryEvaluateInitialBlock(block))
        {
            state.domain = saved;
            state.eventContext = savedEvent;
            return;
        }
    }
    const bool trackTemps =
        (state.domain == ControlDomain::Combinational ||
         state.domain == ControlDomain::Latch ||
         state.domain == ControlDomain::Sequential);
    if (trackTemps)
    {
        state.pushProceduralValues();
    }
    if (const auto* timed = block.getBody().as_if<slang::ast::TimedStatement>())
    {
        if (state.eventContext.edgeSensitive && !state.eventContext.operands.empty())
        {
            state.visitStatement(timed->stmt);
        }
        else
        {
            state.visitStatement(block.getBody());
        }
    }
    else
    {
        state.visitStatement(block.getBody());
    }
    if (trackTemps)
    {
        state.popProceduralValues();
    }
    state.domain = saved;
    state.eventContext = savedEvent;
}

void lowerStmtContinuousAssign(const slang::ast::ContinuousAssignSymbol& assign,
                               StmtLowererState& state)
{
    const ControlDomain saved = state.domain;
    state.domain = ControlDomain::Combinational;
    state.scanExpression(assign.getAssignment());
    state.domain = saved;
}

void lowerStmtMemberSymbol(const slang::ast::Symbol& member, StmtLowererState& state)
{
    if (const auto* net = member.as_if<slang::ast::NetSymbol>())
    {
        if (net->getInitializer())
        {
            state.handleNetInitializer(*net);
            return;
        }
    }
    if (const auto* variable = member.as_if<slang::ast::VariableSymbol>())
    {
        if (variable->getInitializer())
        {
            state.handleVariableInitializer(*variable);
            return;
        }
    }
    if (const auto* continuous = member.as_if<slang::ast::ContinuousAssignSymbol>())
    {
        lowerStmtContinuousAssign(*continuous, state);
        return;
    }
    if (const auto* block = member.as_if<slang::ast::ProceduralBlockSymbol>())
    {
        lowerStmtProceduralBlock(*block, state);
        return;
    }
    if (const auto* generateBlock = member.as_if<slang::ast::GenerateBlockSymbol>())
    {
        lowerStmtGenerateBlock(*generateBlock, state);
        return;
    }
    if (const auto* generateArray = member.as_if<slang::ast::GenerateBlockArraySymbol>())
    {
        lowerStmtGenerateBlockArray(*generateArray, state);
        return;
    }
}

void lowerStmtGenerateBlock(const slang::ast::GenerateBlockSymbol& block, StmtLowererState& state)
{
    if (block.isUninstantiated)
    {
        return;
    }
    for (const slang::ast::Symbol& member : block.members())
    {
        lowerStmtMemberSymbol(member, state);
    }
}

void lowerStmtGenerateBlockArray(const slang::ast::GenerateBlockArraySymbol& array,
                                 StmtLowererState& state)
{
    for (const slang::ast::GenerateBlockSymbol* entry : array.entries)
    {
        if (!entry)
        {
            continue;
        }
        lowerStmtGenerateBlock(*entry, state);
    }
}

PlanSymbolId makeInternalPlanValueSymbol(ModulePlan& plan)
{
    for (;;)
    {
        std::string base = wolvrix::lib::grh::Graph::makeInternalBase("val");
        std::string name = base + "_" + std::to_string(plan.nextInternalSymbol++);
        if (!plan.symbolTable.lookup(name).valid())
        {
            return plan.symbolTable.intern(name);
        }
    }
}

PlanSymbolId makeInoutAuxSymbol(ModulePlan& plan)
{
    return makeInternalPlanValueSymbol(plan);
}

void collectPorts(const slang::ast::InstanceBodySymbol& body, ModulePlan& plan,
                  ConvertDiagnostics* diagnostics)
{
    plan.ports.reserve(body.getPortList().size());

    for (const slang::ast::Symbol* portSymbol : body.getPortList())
    {
        if (!portSymbol)
        {
            continue;
        }

        if (const auto* port = portSymbol->as_if<slang::ast::PortSymbol>())
        {
            if (port->isNullPort || port->name.empty())
            {
                reportUnsupportedPort(*port,
                                      port->isNullPort ? "null ports are not supported"
                                                       : "anonymous ports are not supported",
                                      diagnostics);
                continue;
            }

            PortInfo info;
            info.symbol = plan.symbolTable.intern(port->name);
            switch (port->direction)
            {
            case slang::ast::ArgumentDirection::In:
                info.direction = PortDirection::Input;
                break;
            case slang::ast::ArgumentDirection::Out:
                info.direction = PortDirection::Output;
                break;
            case slang::ast::ArgumentDirection::InOut:
                info.direction = PortDirection::Inout;
                break;
            case slang::ast::ArgumentDirection::Ref:
                reportUnsupportedPort(*port,
                                      std::string("direction ") +
                                          std::string(slang::ast::toString(port->direction)),
                                      diagnostics);
                continue;
            }

            if (info.direction == PortDirection::Inout)
            {
                info.inoutSymbol = PortInfo::InoutBinding{
                    makeInoutAuxSymbol(plan),
                    makeInoutAuxSymbol(plan),
                    makeInoutAuxSymbol(plan)};
            }

            TypeResolution typeInfo = analyzePortType(port->getType(), *port, diagnostics);
            info.width = typeInfo.width;
            info.isSigned = typeInfo.isSigned;
            info.valueType = typeInfo.valueType;

            plan.ports.push_back(std::move(info));
            continue;
        }

        if (const auto* multi = portSymbol->as_if<slang::ast::MultiPortSymbol>())
        {
            reportUnsupportedPort(*multi, "multi-port aggregations", diagnostics);
            continue;
        }

        if (const auto* iface = portSymbol->as_if<slang::ast::InterfacePortSymbol>())
        {
            reportUnsupportedPort(*iface, "interface ports", diagnostics);
            continue;
        }

        reportUnsupportedPort(*portSymbol, "unhandled symbol kind", diagnostics);
    }
}

void collectParameters(const slang::ast::InstanceBodySymbol& body, ModulePlan& plan)
{
    for (const slang::ast::ParameterSymbolBase* paramBase : body.getParameters())
    {
        if (!paramBase || paramBase->symbol.name.empty())
        {
            continue;
        }
        plan.symbolTable.intern(paramBase->symbol.name);
    }

    for (const slang::ast::Symbol& member : body.members())
    {
        if (const auto* param = member.as_if<slang::ast::ParameterSymbol>())
        {
            if (!param->name.empty())
            {
                plan.symbolTable.intern(param->name);
            }
            continue;
        }
        if (const auto* typeParam = member.as_if<slang::ast::TypeParameterSymbol>())
        {
            if (!typeParam->name.empty())
            {
                plan.symbolTable.intern(typeParam->name);
            }
        }
    }
}

void collectSignals(const slang::ast::InstanceBodySymbol& body, ModulePlan& plan,
                    ConvertDiagnostics* diagnostics)
{
    // Generate scope path: symbolPrefix embeds the elaboration round of each
    // enclosing generate-for entry ("gen_loop$3"), groupScope holds the bare
    // block names only ("gen_loop") and keys the per-declaration copy groups.
    struct GenerateScopePath {
        std::string symbolPrefix;
        std::string groupScope;
    };

    auto appendScope = [](const std::string& base, std::string_view component) {
        if (base.empty())
        {
            return std::string(component);
        }
        std::string out = base;
        out.push_back('$');
        out.append(component);
        return out;
    };

    auto recordSignal = [&](const slang::ast::ValueSymbol& symbol, PlanSymbolId id,
                            const GenerateScopePath& scope)
    {
        plan.signalBySlangSymbol.emplace(&symbol, id);
        if (scope.groupScope.empty())
        {
            return;
        }
        std::string key = scope.groupScope;
        key.push_back('\x1f');
        key.append(symbol.name);
        auto [it, inserted] =
            plan.generateGroupByKey.try_emplace(key, plan.generateGroups.size());
        if (inserted)
        {
            GenerateGroupPlan group;
            group.scope = scope.groupScope;
            group.name = std::string(symbol.name);
            plan.generateGroups.push_back(std::move(group));
        }
        plan.generateGroups[it->second].symbols.push_back(id);
    };

    auto internSignal = [&](const slang::ast::ValueSymbol& symbol,
                            const GenerateScopePath& scope) -> PlanSymbolId
    {
        PlanSymbolId id;
        if (scope.symbolPrefix.empty())
        {
            id = plan.symbolTable.intern(symbol.name);
        }
        else
        {
            id = plan.symbolTable.intern(appendScope(scope.symbolPrefix, symbol.name));
        }
        recordSignal(symbol, id, scope);
        return id;
    };

    std::function<void(const slang::ast::NetSymbol&, const GenerateScopePath&)> collectNet =
        [&](const slang::ast::NetSymbol& net, const GenerateScopePath& scope)
    {
        if (net.name.empty())
        {
            if (diagnostics)
            {
                diagnostics->warn(net, "Skipping anonymous net symbol");
            }
            return;
        }
        SignalInfo info;
        info.symbol = internSignal(net, scope);
        info.kind = SignalKind::Net;
        TypeResolution typeInfo = analyzeSignalType(net.getType(), net, diagnostics);
        info.width = typeInfo.width;
        info.isSigned = typeInfo.isSigned;
        info.valueType = typeInfo.valueType;
        info.memoryRows = typeInfo.memoryRows;
        info.packedDims = std::move(typeInfo.packedDims);
        info.unpackedDims = std::move(typeInfo.unpackedDims);
        plan.signals.push_back(std::move(info));
    };

    std::function<void(const slang::ast::VariableSymbol&, const GenerateScopePath&)>
        collectVariable = [&](const slang::ast::VariableSymbol& variable,
                              const GenerateScopePath& scope)
    {
        if (variable.name.empty())
        {
            if (diagnostics)
            {
                diagnostics->warn(variable, "Skipping anonymous variable symbol");
            }
            return;
        }
        if (variable.getType().isEvent())
        {
            if (diagnostics)
            {
                diagnostics->warn(variable, "Skipping event variable symbol");
            }
            return;
        }
        SignalInfo info;
        info.symbol = internSignal(variable, scope);
        info.kind = SignalKind::Variable;
        TypeResolution typeInfo = analyzeSignalType(variable.getType(), variable, diagnostics);
        if (auto packedMemory =
                analyzePackedElementMemoryType(variable.getType(), variable, diagnostics))
        {
            typeInfo.width = packedMemory->first;
            typeInfo.memoryRows = packedMemory->second;
            typeInfo.unpackedDims.clear();
            UnpackedDimInfo dim;
            dim.extent = static_cast<int32_t>(packedMemory->second);
            dim.left = 0;
            dim.right = static_cast<int32_t>(packedMemory->second - 1);
            typeInfo.unpackedDims.push_back(dim);
            typeInfo.packedDims.clear();
        }
        info.width = typeInfo.width;
        info.isSigned = typeInfo.isSigned;
        info.valueType = typeInfo.valueType;
        info.memoryRows = typeInfo.memoryRows;
        info.packedDims = std::move(typeInfo.packedDims);
        info.unpackedDims = std::move(typeInfo.unpackedDims);
        plan.signals.push_back(std::move(info));
    };

    std::function<void(const slang::ast::GenerateBlockSymbol&, const GenerateScopePath&)>
        collectBlock;

    std::function<void(const slang::ast::GenerateBlockArraySymbol&, const GenerateScopePath&)>
        collectArray = [&](const slang::ast::GenerateBlockArraySymbol& array,
                           const GenerateScopePath& scope)
    {
        if (!array.valid)
        {
            return;
        }
        const std::string arrayName =
            array.name.empty() ? array.getExternalName() : std::string(array.name);
        // The group scope names the declaration (no round); the symbol prefix
        // embeds the elaboration round so each copy gets a unique name.
        const std::string arrayGroupScope = appendScope(scope.groupScope, arrayName);
        for (const slang::ast::GenerateBlockSymbol* entry : array.entries)
        {
            if (!entry || entry->isUninstantiated)
            {
                continue;
            }
            int64_t round = static_cast<int64_t>(entry->constructIndex);
            if (entry->arrayIndex)
            {
                if (const std::optional<int64_t> value = entry->arrayIndex->as<int64_t>())
                {
                    round = *value;
                }
            }
            GenerateScopePath entryScope{
                appendScope(scope.symbolPrefix, arrayName + "$" + std::to_string(round)),
                arrayGroupScope};
            collectBlock(*entry, entryScope);
        }
    };

    collectBlock = [&](const slang::ast::GenerateBlockSymbol& block,
                       const GenerateScopePath& scope)
    {
        if (block.isUninstantiated)
        {
            return;
        }
        for (const slang::ast::Symbol& member : block.members())
        {
            if (const auto* net = member.as_if<slang::ast::NetSymbol>())
            {
                collectNet(*net, scope);
                continue;
            }
            if (const auto* variable = member.as_if<slang::ast::VariableSymbol>())
            {
                collectVariable(*variable, scope);
                continue;
            }
            if (const auto* nestedBlock = member.as_if<slang::ast::GenerateBlockSymbol>())
            {
                // Standalone if/case generate block: a single copy, no round.
                const std::string blockName = nestedBlock->name.empty()
                                                  ? nestedBlock->getExternalName()
                                                  : std::string(nestedBlock->name);
                GenerateScopePath childScope{appendScope(scope.symbolPrefix, blockName),
                                             appendScope(scope.groupScope, blockName)};
                collectBlock(*nestedBlock, childScope);
                continue;
            }
            if (const auto* nestedArray =
                    member.as_if<slang::ast::GenerateBlockArraySymbol>())
            {
                collectArray(*nestedArray, scope);
                continue;
            }
        }
    };

    const GenerateScopePath topScope{};
    for (const slang::ast::Symbol& member : body.members())
    {
        if (const auto* net = member.as_if<slang::ast::NetSymbol>())
        {
            collectNet(*net, topScope);
            continue;
        }

        if (const auto* variable = member.as_if<slang::ast::VariableSymbol>())
        {
            collectVariable(*variable, topScope);
            continue;
        }

        if (const auto* generateBlock = member.as_if<slang::ast::GenerateBlockSymbol>())
        {
            collectBlock(*generateBlock, topScope);
            continue;
        }

        if (const auto* generateArray = member.as_if<slang::ast::GenerateBlockArraySymbol>())
        {
            collectArray(*generateArray, topScope);
            continue;
        }
    }
}

PlanSymbolId resolveSimpleSymbolForPlan(const slang::ast::Expression& expr,
                                        const ModulePlan& plan)
{
    if (const auto* assign = expr.as_if<slang::ast::AssignmentExpression>())
    {
        if (assign->isLValueArg())
        {
            return resolveSimpleSymbolForPlan(assign->left(), plan);
        }
        return resolveSimpleSymbolForPlan(assign->right(), plan);
    }
    if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
    {
        return lookupPlanSignal(plan, named->symbol);
    }
    if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
    {
        return resolveSimpleSymbolForPlan(conversion->operand(), plan);
    }
    return {};
}

const SignalInfo* findSignalBySymbol(const ModulePlan& plan, PlanSymbolId symbol)
{
    if (!symbol.valid())
    {
        return nullptr;
    }
    for (const auto& signal : plan.signals)
    {
        if (signal.symbol.index == symbol.index)
        {
            return &signal;
        }
    }
    return nullptr;
}

bool hasInoutSignalBinding(const ModulePlan& plan, PlanSymbolId symbol)
{
    if (!symbol.valid())
    {
        return false;
    }
    for (const auto& info : plan.inoutSignals)
    {
        if (info.symbol.index == symbol.index)
        {
            return true;
        }
    }
    return false;
}

void collectInoutSignals(ModulePlan& plan)
{
    for (const auto& instanceInfo : plan.instances)
    {
        if (!instanceInfo.instance)
        {
            continue;
        }
        const slang::ast::InstanceSymbol& instance = *instanceInfo.instance;
        const slang::ast::InstanceBodySymbol& body = instance.body;
        for (const slang::ast::Symbol* portSymbol : body.getPortList())
        {
            if (!portSymbol)
            {
                continue;
            }
            const auto* port = portSymbol->as_if<slang::ast::PortSymbol>();
            if (!port || port->direction != slang::ast::ArgumentDirection::InOut)
            {
                continue;
            }
            const slang::ast::PortConnection* connection =
                instance.getPortConnection(*port);
            const slang::ast::Expression* expr =
                connection ? connection->getExpression() : nullptr;
            if (!expr || expr->bad())
            {
                continue;
            }
            PlanSymbolId symbol = resolveSimpleSymbolForPlan(*expr, plan);
            if (!symbol.valid())
            {
                continue;
            }
            std::string_view name = plan.symbolTable.text(symbol);
            if (name.empty())
            {
                continue;
            }
            if (const PortInfo* portInfo = findPortByName(plan, name))
            {
                if (portInfo->direction == PortDirection::Inout && portInfo->inoutSymbol)
                {
                    continue;
                }
                continue;
            }
            const SignalInfo* signal = findSignalBySymbol(plan, symbol);
            if (!signal || signal->kind != SignalKind::Net)
            {
                continue;
            }
            if (hasInoutSignalBinding(plan, symbol))
            {
                continue;
            }
            PortInfo::InoutBinding binding{symbol,
                                           makeInoutAuxSymbol(plan),
                                           makeInoutAuxSymbol(plan)};
            plan.inoutSignals.push_back(InoutSignalInfo{symbol, binding});
        }
    }
}

void enqueuePlanKey(ConvertContext& context, const slang::ast::InstanceBodySymbol& body,
                    std::string paramSignature = {})
{
    if (!context.planQueue)
    {
        return;
    }
    if (context.cancelFlag && context.cancelFlag->load(std::memory_order_relaxed))
    {
        return;
    }
    PlanKey key;
    key.definition = &body.getDefinition();
    key.body = &body;
    key.paramSignature = std::move(paramSignature);
    if (context.instanceRegistry && !context.instanceRegistry->trySchedule(key))
    {
        return;
    }
    if (context.taskCounter)
    {
        context.taskCounter->fetch_add(1, std::memory_order_relaxed);
    }
    if (!context.planQueue->tryPush(std::move(key)) && context.taskCounter)
    {
        context.taskCounter->fetch_sub(1, std::memory_order_relaxed);
    }
}

void collectInstance(const slang::ast::InstanceSymbol& instance, ModulePlan& plan,
                     ConvertContext& context)
{
    const slang::ast::InstanceBodySymbol& body = instance.body;

    InstanceInfo info;
    info.instance = &instance;
    info.isBlackbox = isBlackboxBody(body, context.diagnostics);
    std::string_view instanceName = instance.name;
    if (instanceName.empty())
    {
        instanceName = instance.getArrayName();
    }
    info.instanceSymbol = plan.symbolTable.intern(instanceName);

    std::string_view moduleName = body.getDefinition().name;
    if (moduleName.empty())
    {
        moduleName = instance.name;
    }
    info.moduleSymbol = plan.symbolTable.intern(moduleName);
    ParameterSnapshot params = snapshotParameters(body, info.isBlackbox ? &plan : nullptr);
    if (info.isBlackbox)
    {
        info.parameters = std::move(params.parameters);
    }
    info.paramSignature = params.signature;
    plan.instances.push_back(std::move(info));

    enqueuePlanKey(context, body, std::move(params.signature));
}

void collectInstanceArray(const slang::ast::InstanceArraySymbol& array, ModulePlan& plan,
                          ConvertContext& context);
void collectGenerateBlock(const slang::ast::GenerateBlockSymbol& block, ModulePlan& plan,
                          ConvertContext& context);
void collectGenerateBlockArray(const slang::ast::GenerateBlockArraySymbol& array, ModulePlan& plan,
                               ConvertContext& context);

void collectInstanceArray(const slang::ast::InstanceArraySymbol& array, ModulePlan& plan,
                          ConvertContext& context)
{
    for (const slang::ast::Symbol* element : array.elements)
    {
        if (!element)
        {
            continue;
        }

        if (const auto* childInstance = element->as_if<slang::ast::InstanceSymbol>())
        {
            collectInstance(*childInstance, plan, context);
            continue;
        }

        if (const auto* nestedArray = element->as_if<slang::ast::InstanceArraySymbol>())
        {
            collectInstanceArray(*nestedArray, plan, context);
            continue;
        }

        if (const auto* generateBlock = element->as_if<slang::ast::GenerateBlockSymbol>())
        {
            collectGenerateBlock(*generateBlock, plan, context);
            continue;
        }

        if (const auto* generateArray = element->as_if<slang::ast::GenerateBlockArraySymbol>())
        {
            collectGenerateBlockArray(*generateArray, plan, context);
        }
    }
}

void collectGenerateBlock(const slang::ast::GenerateBlockSymbol& block, ModulePlan& plan,
                          ConvertContext& context)
{
    if (block.isUninstantiated)
    {
        return;
    }

    for (const slang::ast::Symbol& member : block.members())
    {
        if (const auto* childInstance = member.as_if<slang::ast::InstanceSymbol>())
        {
            collectInstance(*childInstance, plan, context);
            continue;
        }

        if (const auto* instanceArray = member.as_if<slang::ast::InstanceArraySymbol>())
        {
            collectInstanceArray(*instanceArray, plan, context);
            continue;
        }

        if (const auto* nestedBlock = member.as_if<slang::ast::GenerateBlockSymbol>())
        {
            collectGenerateBlock(*nestedBlock, plan, context);
            continue;
        }

        if (const auto* nestedArray = member.as_if<slang::ast::GenerateBlockArraySymbol>())
        {
            collectGenerateBlockArray(*nestedArray, plan, context);
            continue;
        }
    }
}

void collectGenerateBlockArray(const slang::ast::GenerateBlockArraySymbol& array, ModulePlan& plan,
                               ConvertContext& context)
{
    for (const slang::ast::GenerateBlockSymbol* entry : array.entries)
    {
        if (!entry)
        {
            continue;
        }
        collectGenerateBlock(*entry, plan, context);
    }
}

void collectInstances(const slang::ast::InstanceBodySymbol& body, ModulePlan& plan,
                      ConvertContext& context)
{
    for (const slang::ast::Symbol& member : body.members())
    {
        if (const auto* childInstance = member.as_if<slang::ast::InstanceSymbol>())
        {
            collectInstance(*childInstance, plan, context);
            continue;
        }

        if (const auto* instanceArray = member.as_if<slang::ast::InstanceArraySymbol>())
        {
            collectInstanceArray(*instanceArray, plan, context);
            continue;
        }

        if (const auto* generateBlock = member.as_if<slang::ast::GenerateBlockSymbol>())
        {
            collectGenerateBlock(*generateBlock, plan, context);
            continue;
        }

        if (const auto* generateArray = member.as_if<slang::ast::GenerateBlockArraySymbol>())
        {
            collectGenerateBlockArray(*generateArray, plan, context);
            continue;
        }
    }
}

} // namespace

PlanSymbolId PlanSymbolTable::intern(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    if (auto it = index_.find(text); it != index_.end())
    {
        return it->second;
    }
    storage_.emplace_back(text);
    const std::string_view stored = storage_.back();
    PlanSymbolId id{static_cast<PlanIndex>(storage_.size() - 1)};
    index_.emplace(stored, id);
    return id;
}

PlanSymbolId PlanSymbolTable::lookup(std::string_view text) const
{
    if (text.empty())
    {
        return {};
    }
    if (auto it = index_.find(text); it != index_.end())
    {
        return it->second;
    }
    return {};
}

std::string_view PlanSymbolTable::text(PlanSymbolId id) const
{
    if (!id.valid() || id.index >= storage_.size())
    {
        return {};
    }
    return storage_[id.index];
}

void ConvertDiagnostics::error(const slang::ast::Symbol& symbol, std::string message)
{
    add(ConvertDiagnosticKind::Error, std::move(message), {}, {}, std::string(symbol.name),
        symbol.location.valid() ? std::optional(symbol.location) : std::nullopt);
}

void ConvertDiagnostics::warn(const slang::ast::Symbol& symbol, std::string message)
{
    add(ConvertDiagnosticKind::Warning, std::move(message), {}, {}, std::string(symbol.name),
        symbol.location.valid() ? std::optional(symbol.location) : std::nullopt);
}

void ConvertDiagnostics::error(const slang::SourceLocation& location, std::string message,
                               std::string originSymbol)
{
    add(ConvertDiagnosticKind::Error, std::move(message), {}, {}, std::move(originSymbol),
        location.valid() ? std::optional(location) : std::nullopt);
}

void ConvertDiagnostics::warn(const slang::SourceLocation& location, std::string message,
                              std::string originSymbol)
{
    add(ConvertDiagnosticKind::Warning, std::move(message), {}, {}, std::move(originSymbol),
        location.valid() ? std::optional(location) : std::nullopt);
}

bool PlanCache::tryClaim(const PlanKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        entries_.emplace(key, PlanEntry{PlanStatus::Planning, std::nullopt});
        return true;
    }
    if (it->second.status == PlanStatus::Planning)
    {
        return false;
    }
    if (it->second.status == PlanStatus::Done)
    {
        return false;
    }
    it->second.status = PlanStatus::Planning;
    it->second.plan.reset();
    return true;
}

PlanEntry& PlanCache::getOrCreateEntryLocked(const PlanKey& key)
{
    return entries_[key];
}

PlanEntry* PlanCache::findEntryLocked(const PlanKey& key)
{
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return nullptr;
    }
    return &it->second;
}

const PlanEntry* PlanCache::findEntryLocked(const PlanKey& key) const
{
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        return nullptr;
    }
    return &it->second;
}

void PlanCache::storePlan(const PlanKey& key, ModulePlan plan)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PlanEntry& entry = entries_[key];
    entry.status = PlanStatus::Done;
    entry.plan = std::move(plan);
}

void PlanCache::markFailed(const PlanKey& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PlanEntry& entry = entries_[key];
    entry.status = PlanStatus::Failed;
    entry.plan.reset();
}

std::optional<ModulePlan> PlanCache::findReady(const PlanKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.status != PlanStatus::Done || !it->second.plan)
    {
        return std::nullopt;
    }
    return it->second.plan;
}

void PlanCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

static bool canWriteArtifacts(const PlanEntry& entry)
{
    if (entry.status == PlanStatus::Failed || entry.status == PlanStatus::Pending)
    {
        return false;
    }
    return true;
}

bool PlanCache::setLoweringPlan(const PlanKey& key, LoweringPlan plan)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PlanEntry* entry = findEntryLocked(key);
    if (!entry || !canWriteArtifacts(*entry))
    {
        return false;
    }
    entry->artifacts.loweringPlan = std::move(plan);
    return true;
}

bool PlanCache::setWriteBackPlan(const PlanKey& key, WriteBackPlan plan)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PlanEntry* entry = findEntryLocked(key);
    if (!entry || !canWriteArtifacts(*entry))
    {
        return false;
    }
    entry->artifacts.writeBackPlan = std::move(plan);
    return true;
}

std::optional<LoweringPlan> PlanCache::getLoweringPlan(const PlanKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.artifacts.loweringPlan)
    {
        return std::nullopt;
    }
    return it->second.artifacts.loweringPlan;
}

std::optional<WriteBackPlan> PlanCache::getWriteBackPlan(const PlanKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.artifacts.writeBackPlan)
    {
        return std::nullopt;
    }
    return it->second.artifacts.writeBackPlan;
}

bool PlanCache::withLoweringPlan(const PlanKey& key,
                                 const std::function<void(const LoweringPlan&)>& fn) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const PlanEntry* entry = findEntryLocked(key);
    if (!entry || !entry->artifacts.loweringPlan || !fn)
    {
        return false;
    }
    fn(*entry->artifacts.loweringPlan);
    return true;
}

bool PlanCache::withWriteBackPlan(const PlanKey& key,
                                  const std::function<void(const WriteBackPlan&)>& fn) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const PlanEntry* entry = findEntryLocked(key);
    if (!entry || !entry->artifacts.writeBackPlan || !fn)
    {
        return false;
    }
    fn(*entry->artifacts.writeBackPlan);
    return true;
}

bool PlanCache::withLoweringPlanMut(const PlanKey& key,
                                    const std::function<void(LoweringPlan&)>& fn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PlanEntry* entry = findEntryLocked(key);
    if (!entry || !entry->artifacts.loweringPlan || !fn || !canWriteArtifacts(*entry))
    {
        return false;
    }
    fn(*entry->artifacts.loweringPlan);
    return true;
}

bool PlanCache::withWriteBackPlanMut(const PlanKey& key,
                                     const std::function<void(WriteBackPlan&)>& fn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PlanEntry* entry = findEntryLocked(key);
    if (!entry || !entry->artifacts.writeBackPlan || !fn || !canWriteArtifacts(*entry))
    {
        return false;
    }
    fn(*entry->artifacts.writeBackPlan);
    return true;
}

void PlanTaskQueue::push(PlanKey key)
{
    (void)tryPush(std::move(key));
}

bool PlanTaskQueue::tryPush(PlanKey key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
    {
        return false;
    }
    queue_.push_back(std::move(key));
    cv_.notify_one();
    return true;
}

bool PlanTaskQueue::tryPop(PlanKey& out)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty())
    {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

bool PlanTaskQueue::waitPop(PlanKey& out, const std::atomic<bool>* cancelFlag)
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&]() {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
        {
            return true;
        }
        return closed_ || !queue_.empty();
    });
    if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
    {
        return false;
    }
    if (queue_.empty())
    {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void PlanTaskQueue::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

std::size_t PlanTaskQueue::drain()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t dropped = queue_.size();
    queue_.clear();
    cv_.notify_all();
    return dropped;
}

bool PlanTaskQueue::closed() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::size_t PlanTaskQueue::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void PlanTaskQueue::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    closed_ = false;
    cv_.notify_all();
}

ModulePlan ModulePlanner::plan(const slang::ast::InstanceBodySymbol& body)
{
    ModulePlan plan;
    plan.body = &body;
    std::string_view moduleName = body.name;
    if (moduleName.empty())
    {
        moduleName = body.getDefinition().name;
    }
    plan.moduleSymbol = plan.symbolTable.intern(moduleName);
    collectParameters(body, plan);
    collectPorts(body, plan, context_.diagnostics);
    collectSignals(body, plan, context_.diagnostics);
    collectInstances(body, plan, context_);
    collectInoutSignals(plan);
    return plan;
}

void StmtLowererPass::lower(ModulePlan& plan, LoweringPlan& lowering)
{
    if (!plan.body)
    {
        return;
    }

    lowering.values.clear();
    lowering.tempSymbols.clear();
    lowering.writes.clear();
    lowering.loweredStmts.clear();
    lowering.dpiImports.clear();
    lowering.memoryReads.clear();
    lowering.memoryWrites.clear();
    lowering.memoryFills.clear();

    StmtLowererState state(plan, context_.diagnostics, lowering,
                           context_.options.maxLoopIterations);
    for (const slang::ast::Symbol& member : plan.body->members())
    {
        lowerStmtMemberSymbol(member, state);
    }
}

namespace {

std::optional<int64_t> evalConstInt(const ModulePlan& plan, const LoweringPlan& lowering,
                                    ExprNodeId id);

class WriteBackBuilder {
public:
    WriteBackBuilder(ModulePlan& plan, LoweringPlan& lowering)
        : plan_(plan), lowering_(lowering)
    {
    }

    ExprNodeId ensureGuardExpr(ExprNodeId guard, slang::SourceLocation location)
    {
        if (guard != kInvalidPlanIndex)
        {
            return guard;
        }
        if (constOne_.has_value())
        {
            return *constOne_;
        }
        ExprNodeId id = addConstantLiteral("1'b1", location);
        constOne_ = id;
        return id;
    }

    ExprNodeId makeLogicOr(ExprNodeId lhs, ExprNodeId rhs, slang::SourceLocation location)
    {
        if (lhs == kInvalidPlanIndex)
        {
            return rhs;
        }
        if (rhs == kInvalidPlanIndex)
        {
            return lhs;
        }
        return makeOperation(wolvrix::lib::grh::OperationKind::kLogicOr, {lhs, rhs}, location);
    }

    ExprNodeId makeMux(ExprNodeId cond, ExprNodeId lhs, ExprNodeId rhs,
                       slang::SourceLocation location)
    {
        if (cond == kInvalidPlanIndex || lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        int32_t widthHint = 0;
        if (lhs < lowering_.values.size())
        {
            widthHint = lowering_.values[lhs].widthHint;
        }
        if (widthHint <= 0 && rhs < lowering_.values.size())
        {
            widthHint = lowering_.values[rhs].widthHint;
        }
        return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kMux,
                                      {cond, lhs, rhs}, widthHint, location);
    }

    ExprNodeId addSymbol(PlanSymbolId symbol, slang::SourceLocation location)
    {
        if (!symbol.valid())
        {
            return kInvalidPlanIndex;
        }
        ExprNode node;
        node.kind = ExprNodeKind::Symbol;
        node.symbol = symbol;
        node.location = location;
        return addNode(std::move(node));
    }

    ExprNodeId addConstantLiteral(std::string literal, slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Constant;
        node.literal = std::move(literal);
        node.location = location;
        return addNode(std::move(node));
    }

    ExprNodeId addConstantLiteralWithWidth(std::string literal, int32_t widthHint,
                                           slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Constant;
        node.literal = std::move(literal);
        node.location = location;
        if (widthHint > 0)
        {
            node.widthHint = widthHint;
        }
        return addNode(std::move(node));
    }

    ExprNodeId makeOperation(wolvrix::lib::grh::OperationKind op, std::vector<ExprNodeId> operands,
                             slang::SourceLocation location)
    {
        return makeOperationWithWidth(op, std::move(operands), 0, location);
    }

    ExprNodeId makeOperationWithWidth(wolvrix::lib::grh::OperationKind op,
                                      std::vector<ExprNodeId> operands,
                                      int32_t widthHint,
                                      slang::SourceLocation location)
    {
        for (ExprNodeId operand : operands)
        {
            if (operand == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
        }
        ExprNode node;
        node.kind = ExprNodeKind::Operation;
        node.op = op;
        node.operands = std::move(operands);
        node.location = location;
        node.tempSymbol = makeTempSymbol();
        if (widthHint > 0)
        {
            node.widthHint = widthHint;
        }
        return addNode(std::move(node));
    }

    ExprNodeId makeSliceDynamic(ExprNodeId value, ExprNodeId index, int32_t width,
                                slang::SourceLocation location)
    {
        return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kSliceDynamic, {value, index},
                                      width, location);
    }

    ExprNodeId makeConcat(std::vector<ExprNodeId> operands, int32_t width,
                          slang::SourceLocation location)
    {
        return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kConcat, std::move(operands),
                                      width, location);
    }

    ExprNodeId resizeValueToWidth(ExprNodeId value, int32_t targetWidth,
                                  bool signExtend, slang::SourceLocation location)
    {
        if (value == kInvalidPlanIndex || targetWidth <= 0)
        {
            return value;
        }
        int32_t sourceWidth = 0;
        if (value < lowering_.values.size())
        {
            sourceWidth = lowering_.values[value].widthHint;
        }
        if (sourceWidth == targetWidth)
        {
            return value;
        }
        if (sourceWidth <= 0)
        {
            ExprNode castNode;
            castNode.kind = ExprNodeKind::Operation;
            castNode.op = wolvrix::lib::grh::OperationKind::kAssign;
            castNode.operands = {value};
            castNode.location = location;
            castNode.tempSymbol = makeTempSymbol();
            castNode.widthHint = targetWidth;
            castNode.isSigned = signExtend;
            if (value < lowering_.values.size())
            {
                castNode.valueType = lowering_.values[value].valueType;
            }
            return addNode(std::move(castNode));
        }
        if (targetWidth < sourceWidth)
        {
            ExprNodeId zeroIndex = addConstantLiteral("0", location);
            return makeSliceDynamic(value, zeroIndex, targetWidth, location);
        }
        const int32_t padWidth = targetWidth - sourceWidth;
        ExprNodeId pad = kInvalidPlanIndex;
        if (signExtend && sourceWidth > 0)
        {
            ExprNodeId signIndex =
                addConstantLiteral(std::to_string(sourceWidth - 1), location);
            ExprNodeId signBit = makeSliceDynamic(value, signIndex, 1, location);
            ExprNode countNode;
            countNode.kind = ExprNodeKind::Constant;
            countNode.literal = std::to_string(padWidth);
            countNode.location = location;
            countNode.widthHint = 32;
            ExprNodeId countId = addNode(std::move(countNode));
            ExprNode repNode;
            repNode.kind = ExprNodeKind::Operation;
            repNode.op = wolvrix::lib::grh::OperationKind::kReplicate;
            repNode.operands = {countId, signBit};
            repNode.location = location;
            repNode.tempSymbol = makeTempSymbol();
            repNode.widthHint = padWidth;
            pad = addNode(std::move(repNode));
        }
        else
        {
            ExprNode padNode;
            padNode.kind = ExprNodeKind::Constant;
            padNode.literal = std::to_string(padWidth) + "'b0";
            padNode.location = location;
            padNode.widthHint = padWidth;
            pad = addNode(std::move(padNode));
        }
        if (pad == kInvalidPlanIndex)
        {
            return value;
        }
        ExprNode concatNode;
        concatNode.kind = ExprNodeKind::Operation;
        concatNode.op = wolvrix::lib::grh::OperationKind::kConcat;
        concatNode.operands = {pad, value};
        concatNode.location = location;
        concatNode.tempSymbol = makeTempSymbol();
        concatNode.widthHint = targetWidth;
        return addNode(std::move(concatNode));
    }

private:
    PlanSymbolId makeTempSymbol()
    {
        PlanSymbolId id = makeInternalPlanValueSymbol(plan_);
        lowering_.tempSymbols.push_back(id);
        return id;
    }

    ExprNodeId addNode(ExprNode node)
    {
        const ExprNodeId id = static_cast<ExprNodeId>(lowering_.values.size());
        lowering_.values.push_back(std::move(node));
        return id;
    }

    ModulePlan& plan_;
    LoweringPlan& lowering_;
    std::optional<ExprNodeId> constOne_;
};

struct WriteBackGroup {
    PlanSymbolId target;
    ControlDomain domain = ControlDomain::Unknown;
    std::vector<EventEdge> eventEdges;
    std::vector<ExprNodeId> eventOperands;
    std::vector<const LoweredStmt*> writes;
};

bool matchWriteGroup(const WriteBackGroup& group, PlanSymbolId target, ControlDomain domain,
                     const std::vector<EventEdge>& edges,
                     const std::vector<ExprNodeId>& operands)
{
    return group.target.index == target.index && group.domain == domain &&
           group.eventEdges == edges && group.eventOperands == operands;
}

} // namespace

WriteBackPlan WriteBackPass::lower(ModulePlan& plan, LoweringPlan& lowering)
{
    WriteBackPlan result;
    if (!plan.body)
    {
        return result;
    }

    std::vector<SignalId> signalBySymbol(plan.symbolTable.size(), kInvalidPlanIndex);
    for (SignalId i = 0; i < static_cast<SignalId>(plan.signals.size()); ++i)
    {
        const PlanSymbolId id = plan.signals[i].symbol;
        if (id.valid() && id.index < signalBySymbol.size())
        {
            signalBySymbol[id.index] = i;
        }
    }
    std::vector<int32_t> widthBySymbol(plan.symbolTable.size(), 0);
    for (const auto& port : plan.ports)
    {
        if (!port.symbol.valid() || port.symbol.index >= widthBySymbol.size())
        {
            continue;
        }
        widthBySymbol[port.symbol.index] = port.width;
        if (port.direction == PortDirection::Inout && port.inoutSymbol)
        {
            const auto& binding = *port.inoutSymbol;
            if (binding.inSymbol.valid() && binding.inSymbol.index < widthBySymbol.size())
            {
                widthBySymbol[binding.inSymbol.index] = port.width;
            }
            if (binding.outSymbol.valid() && binding.outSymbol.index < widthBySymbol.size())
            {
                widthBySymbol[binding.outSymbol.index] = port.width;
            }
            if (binding.oeSymbol.valid() && binding.oeSymbol.index < widthBySymbol.size())
            {
                widthBySymbol[binding.oeSymbol.index] = port.width;
            }
        }
    }
    for (const auto& signal : plan.signals)
    {
        if (!signal.symbol.valid() || signal.symbol.index >= widthBySymbol.size())
        {
            continue;
        }
        if (widthBySymbol[signal.symbol.index] == 0)
        {
            int32_t width = signal.width;
            if (isFlattenedNetArray(signal))
            {
                width = static_cast<int32_t>(flattenedNetWidth(signal));
            }
            else if (isPackedAggregateVariable(signal))
            {
                width = static_cast<int32_t>(flattenedAggregateWidth(signal));
            }
            widthBySymbol[signal.symbol.index] = width;
        }
    }
    std::vector<const slang::ast::Type*> typeBySymbol(plan.symbolTable.size(), nullptr);
    for (const auto* portSymbol : plan.body->getPortList())
    {
        const auto* port = portSymbol ? portSymbol->as_if<slang::ast::PortSymbol>() : nullptr;
        if (!port || port->name.empty())
        {
            continue;
        }
        const PlanSymbolId id = plan.symbolTable.lookup(port->name);
        if (!id.valid() || id.index >= typeBySymbol.size())
        {
            continue;
        }
        typeBySymbol[id.index] = &port->getType();
    }
    for (const slang::ast::Symbol& member : plan.body->members())
    {
        const auto* net = member.as_if<slang::ast::NetSymbol>();
        const auto* variable = member.as_if<slang::ast::VariableSymbol>();
        const slang::ast::ValueSymbol* valueSymbol =
            net ? static_cast<const slang::ast::ValueSymbol*>(net)
                : static_cast<const slang::ast::ValueSymbol*>(variable);
        if (!valueSymbol || valueSymbol->name.empty())
        {
            continue;
        }
        const PlanSymbolId id = plan.symbolTable.lookup(valueSymbol->name);
        if (!id.valid() || id.index >= typeBySymbol.size())
        {
            continue;
        }
        typeBySymbol[id.index] = &valueSymbol->getType();
    }
    for (const auto& port : plan.ports)
    {
        if (port.direction != PortDirection::Inout || !port.inoutSymbol)
        {
            continue;
        }
        const slang::ast::Type* portType = nullptr;
        if (port.symbol.valid() && port.symbol.index < typeBySymbol.size())
        {
            portType = typeBySymbol[port.symbol.index];
        }
        if (!portType)
        {
            continue;
        }
        const auto& binding = *port.inoutSymbol;
        if (binding.inSymbol.valid() && binding.inSymbol.index < typeBySymbol.size())
        {
            typeBySymbol[binding.inSymbol.index] = portType;
        }
        if (binding.outSymbol.valid() && binding.outSymbol.index < typeBySymbol.size())
        {
            typeBySymbol[binding.outSymbol.index] = portType;
        }
        if (binding.oeSymbol.valid() && binding.oeSymbol.index < typeBySymbol.size())
        {
            typeBySymbol[binding.oeSymbol.index] = portType;
        }
    }

    std::vector<uint8_t> indexedAggregateAccess(plan.symbolTable.size(), 0);
    auto addressCountForSignal = [&](SignalId signal) -> std::size_t {
        if (signal == kInvalidPlanIndex ||
            signal >= static_cast<SignalId>(plan.signals.size()))
        {
            return 1;
        }
        const auto& dims = plan.signals[signal].unpackedDims;
        return dims.empty() ? 1 : dims.size();
    };
    auto markIndexedAggregate = [&](PlanSymbolId symbol, std::size_t indexCount) {
        if (!symbol.valid() || symbol.index >= signalBySymbol.size() ||
            symbol.index >= indexedAggregateAccess.size())
        {
            return;
        }
        SignalId signal = signalBySymbol[symbol.index];
        if (signal == kInvalidPlanIndex ||
            signal >= static_cast<SignalId>(plan.signals.size()) ||
            !isPackedAggregateVariable(plan.signals[signal]))
        {
            return;
        }
        if (indexCount >= addressCountForSignal(signal))
        {
            indexedAggregateAccess[symbol.index] = 1;
        }
    };
    auto matchIndexedAggregateRead = [&](ExprNodeId id, PlanSymbolId& symbol) -> bool {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return false;
        }
        ExprNodeId current = id;
        std::size_t indexCount = 0;
        while (current != kInvalidPlanIndex && current < lowering.values.size())
        {
            const ExprNode& node = lowering.values[current];
            if (node.kind == ExprNodeKind::Operation &&
                node.op == wolvrix::lib::grh::OperationKind::kAssign &&
                node.operands.size() == 1)
            {
                current = node.operands.front();
                continue;
            }
            const bool isSliceAddress =
                node.kind == ExprNodeKind::Operation &&
                (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic ||
                 node.op == wolvrix::lib::grh::OperationKind::kSliceArray);
            if (!isSliceAddress || node.operands.size() < 2)
            {
                break;
            }
            ++indexCount;
            current = node.operands.front();
        }
        if (current == kInvalidPlanIndex || current >= lowering.values.size())
        {
            return false;
        }
        const ExprNode& node = lowering.values[current];
        if (node.kind != ExprNodeKind::Symbol)
        {
            return false;
        }
        if (!node.symbol.valid() || node.symbol.index >= signalBySymbol.size())
        {
            return false;
        }
        SignalId signal = signalBySymbol[node.symbol.index];
        if (signal == kInvalidPlanIndex ||
            signal >= static_cast<SignalId>(plan.signals.size()) ||
            !isPackedAggregateVariable(plan.signals[signal]) ||
            indexCount < addressCountForSignal(signal))
        {
            return false;
        }
        symbol = node.symbol;
        return true;
    };
    auto visitExprForIndexedAggregate = [&](auto&& self, ExprNodeId id,
                                            std::unordered_set<ExprNodeId>& seen) -> void {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return;
        }
        if (!seen.insert(id).second)
        {
            return;
        }
        PlanSymbolId symbol;
        if (matchIndexedAggregateRead(id, symbol))
        {
            markIndexedAggregate(symbol, addressCountForSignal(signalBySymbol[symbol.index]));
        }
        const ExprNode& node = lowering.values[id];
        if (node.kind != ExprNodeKind::Operation)
        {
            return;
        }
        for (ExprNodeId operand : node.operands)
        {
            self(self, operand, seen);
        }
    };
    for (const auto& stmt : lowering.loweredStmts)
    {
        if (stmt.kind != LoweredStmtKind::Write || stmt.write.isXmr)
        {
            continue;
        }
        const WriteIntent& write = stmt.write;
        if (write.target.valid() && write.target.index < signalBySymbol.size())
        {
            markIndexedAggregate(write.target, write.slices.size());
        }
        std::unordered_set<ExprNodeId> seen;
        visitExprForIndexedAggregate(visitExprForIndexedAggregate, write.value, seen);
        visitExprForIndexedAggregate(visitExprForIndexedAggregate, write.guard, seen);
    }

    std::vector<WriteBackGroup> groups;
    groups.reserve(lowering.loweredStmts.size());

    for (const auto& stmt : lowering.loweredStmts)
    {
        if (stmt.kind != LoweredStmtKind::Write)
        {
            continue;
        }
        const WriteIntent& write = stmt.write;
        if (write.isXmr)
        {
            continue;
        }
        if (!write.target.valid())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(write.location, "Write-back target missing symbol");
            }
            continue;
        }
        SignalId signalId = kInvalidPlanIndex;
        if (write.target.index < signalBySymbol.size())
        {
            signalId = signalBySymbol[write.target.index];
        }
        if (signalId != kInvalidPlanIndex && plan.signals[signalId].memoryRows > 0 &&
            plan.signals[signalId].kind != SignalKind::Net &&
            (!isPackedAggregateVariable(plan.signals[signalId]) ||
             (write.target.index < indexedAggregateAccess.size() &&
              indexedAggregateAccess[write.target.index])))
        {
            continue;
        }
        if (write.value == kInvalidPlanIndex)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(write.location, "Write-back missing RHS expression");
            }
            continue;
        }
        if (stmt.eventEdges.size() != stmt.eventOperands.size())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(write.location,
                                           "Skipping write with mismatched event binding");
            }
            continue;
        }
        if (write.domain == ControlDomain::Sequential &&
            (stmt.eventEdges.empty() || stmt.eventOperands.empty()))
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(
                    write.location,
                    "Skipping sequential write without edge-sensitive timing control; initial/final assignments are ignored");
            }
            continue;
        }

        bool matched = false;
        for (auto& group : groups)
        {
            if (matchWriteGroup(group, write.target, write.domain,
                                stmt.eventEdges, stmt.eventOperands))
            {
                group.writes.push_back(&stmt);
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            WriteBackGroup group;
            group.target = write.target;
            group.domain = write.domain;
            group.eventEdges = stmt.eventEdges;
            group.eventOperands = stmt.eventOperands;
            group.writes.push_back(&stmt);
            groups.push_back(std::move(group));
        }
    }

    WriteBackBuilder builder(plan, lowering);
    result.entries.reserve(groups.size());
    std::vector<uint8_t> resultEntryHasSlices;
    std::vector<ControlDomain> resultEntryOriginalDomain;
    resultEntryHasSlices.reserve(groups.size());
    resultEntryOriginalDomain.reserve(groups.size());

    struct SliceSelection {
        bool isStatic = false;
        ExprNodeId indexExpr = kInvalidPlanIndex;
        int64_t low = 0;
        int64_t width = 0;
        const slang::ast::Type* subType = nullptr;
        int64_t subWidth = 0;
    };

    auto resolveMemberSlice =
        [&](const slang::ast::Type* baseType, PlanSymbolId member, SliceSelection& out) -> bool {
        if (!baseType || !member.valid())
        {
            return false;
        }
        const slang::ast::Type& canonical = baseType->getCanonicalType();
        const slang::ast::Scope* scope = nullptr;
        if (canonical.kind == slang::ast::SymbolKind::PackedStructType)
        {
            scope = &canonical.as<slang::ast::PackedStructType>();
        }
        else if (canonical.kind == slang::ast::SymbolKind::PackedUnionType)
        {
            scope = &canonical.as<slang::ast::PackedUnionType>();
        }
        if (!scope)
        {
            return false;
        }

        std::string_view memberName = plan.symbolTable.text(member);
        const slang::ast::FieldSymbol* field = nullptr;
        for (const auto& candidate : scope->membersOfType<slang::ast::FieldSymbol>())
        {
            if (candidate.name == memberName)
            {
                field = &candidate;
                break;
            }
        }
        if (!field)
        {
            return false;
        }

        const uint64_t widthRaw = computeFixedWidth(field->getType(), *plan.body,
                                                    context_.diagnostics);
        const int32_t width = clampWidth(widthRaw, *plan.body, context_.diagnostics,
                                         "Member select");
        if (width <= 0)
        {
            return false;
        }

        out.isStatic = true;
        out.low = static_cast<int64_t>(field->bitOffset);
        out.width = width;
        out.subType = &field->getType();
        out.subWidth = width;
        return true;
    };

    auto getPackedIndexOffset = [&](const slang::ast::Type* baseType) -> int64_t {
        if (!baseType)
        {
            return 0;
        }
        const auto& canonical = baseType->getCanonicalType();
        if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
        {
            return 0;
        }
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        return std::min<int64_t>(packed.range.left, packed.range.right);
    };

    auto getPackedArrayElementSlice =
        [&](const slang::ast::Type* baseType)
        -> std::optional<std::pair<int64_t, int64_t>> {
        if (!baseType || !plan.body)
        {
            return std::nullopt;
        }
        const auto& canonical = baseType->getCanonicalType();
        if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
        {
            return std::nullopt;
        }
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        const uint64_t widthRaw =
            computeFixedWidth(packed.elementType, *plan.body, context_.diagnostics);
        if (widthRaw == 0)
        {
            return std::nullopt;
        }
        const uint64_t totalWidthRaw =
            computeFixedWidth(canonical, *plan.body, context_.diagnostics);
        if (totalWidthRaw <= widthRaw || widthRaw <= 1)
        {
            return std::nullopt;
        }
        const int64_t elementWidth =
            widthRaw > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
                ? static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                : static_cast<int64_t>(widthRaw);
        if (elementWidth <= 0)
        {
            return std::nullopt;
        }
        const int64_t offset = std::min<int64_t>(packed.range.left, packed.range.right);
        return std::make_pair(offset, elementWidth);
    };

    auto applyPackedIndexOffset = [&](ExprNodeId indexExpr, int64_t offset,
                                      slang::SourceLocation location) -> ExprNodeId {
        if (indexExpr == kInvalidPlanIndex || offset == 0)
        {
            return indexExpr;
        }
        int32_t widthHint = 0;
        if (indexExpr < lowering.values.size())
        {
            widthHint = lowering.values[indexExpr].widthHint;
        }
        if (widthHint <= 0)
        {
            widthHint = 32;
        }
        ExprNodeId offsetId =
            builder.addConstantLiteralWithWidth(std::to_string(offset), widthHint,
                                                location);
        return builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kSub,
                                              {indexExpr, offsetId}, widthHint,
                                              location);
    };

    auto applyPackedElementIndex =
        [&](ExprNodeId indexExpr, int64_t offset, int64_t elementWidth,
            slang::SourceLocation location) -> ExprNodeId {
        ExprNodeId adjusted = applyPackedIndexOffset(indexExpr, offset, location);
        if (adjusted == kInvalidPlanIndex || elementWidth <= 1)
        {
            return adjusted;
        }
        int32_t widthHint = 0;
        if (adjusted < lowering.values.size())
        {
            widthHint = lowering.values[adjusted].widthHint;
        }
        if (widthHint <= 0)
        {
            widthHint = 32;
        }
        int32_t elementWidthHint = 0;
        for (int64_t temp = elementWidth; temp > 0; temp >>= 1)
        {
            ++elementWidthHint;
        }
        if (elementWidthHint <= 0)
        {
            elementWidthHint = 1;
        }
        const int32_t constWidthHint =
            std::max(elementWidthHint, widthHint);
        int64_t mulWidthHint = static_cast<int64_t>(widthHint) + elementWidthHint;
        if (mulWidthHint > std::numeric_limits<int32_t>::max())
        {
            mulWidthHint = std::numeric_limits<int32_t>::max();
        }
        ExprNodeId widthId = builder.addConstantLiteralWithWidth(
            std::to_string(elementWidth), constWidthHint, location);
        return builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kMul,
                                              {adjusted, widthId},
                                              static_cast<int32_t>(mulWidthHint),
                                              location);
    };

    auto resolveSliceSelection =
        [&](const WriteSlice& slice, const slang::ast::Type* baseType,
            int64_t baseWidth, SliceSelection& out) -> bool {
        out = {};
        const int64_t packedOffset = getPackedIndexOffset(baseType);
        switch (slice.kind)
        {
        case WriteSliceKind::MemberSelect:
            if (!resolveMemberSlice(baseType, slice.member, out))
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->error(slice.location,
                                               "Unsupported member select in write-back");
                }
                return false;
            }
            break;
        case WriteSliceKind::BitSelect: {
            if (slice.index == kInvalidPlanIndex)
            {
                return false;
            }
            auto indexConst = evalConstInt(plan, lowering, slice.index);
            const auto packedElement = getPackedArrayElementSlice(baseType);
            out.width = packedElement ? packedElement->second : 1;
            out.subWidth = out.width;
            if (indexConst)
            {
                out.isStatic = true;
                if (packedElement)
                {
                    out.low = (*indexConst - packedElement->first) *
                              packedElement->second;
                }
                else
                {
                    out.low = *indexConst;
                }
            }
            else
            {
                if (packedElement)
                {
                    out.indexExpr = applyPackedElementIndex(
                        slice.index, packedElement->first,
                        packedElement->second, slice.location);
                }
                else
                {
                    out.indexExpr = slice.index;
                }
            }
            break;
        }
        case WriteSliceKind::RangeSelect: {
            if (slice.left == kInvalidPlanIndex || slice.right == kInvalidPlanIndex)
            {
                return false;
            }
            if (slice.rangeKind == WriteRangeKind::Simple)
            {
                auto leftConst = evalConstInt(plan, lowering, slice.left);
                auto rightConst = evalConstInt(plan, lowering, slice.right);
                if (!leftConst || !rightConst)
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->error(
                            slice.location,
                            "Dynamic range select in write-back is unsupported");
                    }
                    return false;
                }
                const int64_t lo = std::min(*leftConst, *rightConst);
                const int64_t hi = std::max(*leftConst, *rightConst);
                out.isStatic = true;
                out.low = lo;
                out.width = hi - lo + 1;
                out.subWidth = out.width;
                break;
            }
            auto widthConst = evalConstInt(plan, lowering, slice.right);
            if (!widthConst || *widthConst <= 0)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->error(
                        slice.location,
                        "Indexed part-select width must be constant in write-back");
                }
                return false;
            }
            out.width = *widthConst;
            out.subWidth = out.width;
            auto baseConst = evalConstInt(plan, lowering, slice.left);
            if (baseConst)
            {
                out.isStatic = true;
                if (slice.rangeKind == WriteRangeKind::IndexedUp)
                {
                    out.low = *baseConst;
                }
                else
                {
                    out.low = *baseConst - out.width + 1;
                }
            }
            else
            {
                ExprNodeId indexExpr = slice.left;
                if (slice.rangeKind == WriteRangeKind::IndexedDown && out.width > 1)
                {
                    ExprNodeId offset =
                        builder.addConstantLiteral(std::to_string(out.width - 1),
                                                   slice.location);
                    indexExpr =
                        builder.makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                              {slice.left, offset}, slice.location);
                }
                out.indexExpr = indexExpr;
            }
            break;
        }
        default:
            return false;
        }

        if ((slice.kind == WriteSliceKind::BitSelect ||
             slice.kind == WriteSliceKind::RangeSelect) &&
            packedOffset != 0 &&
            !(slice.kind == WriteSliceKind::BitSelect &&
              getPackedArrayElementSlice(baseType).has_value()))
        {
            if (out.isStatic)
            {
                out.low -= packedOffset;
            }
            else
            {
                out.indexExpr =
                    applyPackedIndexOffset(out.indexExpr, packedOffset, slice.location);
                if (out.indexExpr == kInvalidPlanIndex)
                {
                    return false;
                }
            }
        }

        if (out.width <= 0)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(slice.location,
                                           "Write-back slice width must be positive");
            }
            return false;
        }
        if (baseWidth > 0 && out.isStatic)
        {
            if (out.low < 0 || out.low + out.width > baseWidth)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        slice.location,
                        "Write-back slice exceeds target width; clamping is not supported");
                }
                return false;
            }
        }
        if (!out.isStatic && out.indexExpr == kInvalidPlanIndex)
        {
            return false;
        }
        if (!out.isStatic && baseWidth > 0 && out.width > baseWidth)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(
                    slice.location,
                    "Write-back slice width exceeds target width");
            }
        }
        return true;
    };

    auto buildSliceExpr = [&](ExprNodeId base, const SliceSelection& slice,
                              slang::SourceLocation location) -> ExprNodeId {
        if (slice.width <= 0)
        {
            return kInvalidPlanIndex;
        }
        if (slice.isStatic && slice.low == 0 && slice.width > 0)
        {
            // Optional fast path: slice covering full width handled by caller.
        }
        ExprNodeId indexExpr = slice.indexExpr;
        if (slice.isStatic)
        {
            indexExpr = builder.addConstantLiteral(std::to_string(slice.low), location);
        }
        return builder.makeSliceDynamic(base, indexExpr,
                                        static_cast<int32_t>(slice.width), location);
    };

    auto spliceStatic = [&](ExprNodeId base, const SliceSelection& slice, ExprNodeId value,
                            int64_t baseWidth,
                            slang::SourceLocation location) -> ExprNodeId {
        if (slice.width == baseWidth && slice.low == 0)
        {
            return value;
        }
        const int64_t hi = slice.low + slice.width - 1;
        const int64_t upperWidth = baseWidth - (hi + 1);
        const int64_t lowerWidth = slice.low;
        std::vector<ExprNodeId> operands;
        operands.reserve(3);
        if (upperWidth > 0)
        {
            ExprNodeId upperIndex =
                builder.addConstantLiteral(std::to_string(hi + 1), location);
            ExprNodeId upper =
                builder.makeSliceDynamic(base, upperIndex,
                                         static_cast<int32_t>(upperWidth), location);
            operands.push_back(upper);
        }
        operands.push_back(value);
        if (lowerWidth > 0)
        {
            ExprNodeId lowerIndex = builder.addConstantLiteral("0", location);
            ExprNodeId lower =
                builder.makeSliceDynamic(base, lowerIndex,
                                         static_cast<int32_t>(lowerWidth), location);
            operands.push_back(lower);
        }
        if (operands.size() == 1)
        {
            return operands.front();
        }
        return builder.makeConcat(std::move(operands),
                                  static_cast<int32_t>(baseWidth), location);
    };

    auto spliceDynamic = [&](ExprNodeId base, const SliceSelection& slice, ExprNodeId value,
                             int64_t baseWidth,
                             slang::SourceLocation location) -> ExprNodeId {
        int32_t widthHint = baseWidth > 0
                                ? static_cast<int32_t>(
                                      std::min<int64_t>(baseWidth,
                                                        std::numeric_limits<int32_t>::max()))
                                : 0;
        ExprNodeId one = builder.addConstantLiteralWithWidth("1", widthHint, location);
        ExprNodeId widthLiteral =
            builder.addConstantLiteral(std::to_string(slice.width), location);
        ExprNodeId shifted =
            builder.makeOperation(wolvrix::lib::grh::OperationKind::kShl,
                                  {one, widthLiteral}, location);
        ExprNodeId ones =
            builder.makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                  {shifted, one}, location);
        ExprNodeId mask =
            builder.makeOperation(wolvrix::lib::grh::OperationKind::kShl,
                                  {ones, slice.indexExpr}, location);
        ExprNodeId inverted =
            builder.makeOperation(wolvrix::lib::grh::OperationKind::kNot, {mask}, location);
        ExprNodeId masked =
            builder.makeOperation(wolvrix::lib::grh::OperationKind::kAnd, {base, inverted}, location);
        ExprNodeId extendedValue = value;
        if (baseWidth > slice.width)
        {
            const int64_t padWidth = baseWidth - slice.width;
            const std::string zeroLiteral =
                std::to_string(padWidth) + "'b0";
            ExprNodeId zeros = builder.addConstantLiteralWithWidth(
                zeroLiteral,
                static_cast<int32_t>(std::min<int64_t>(
                    padWidth, std::numeric_limits<int32_t>::max())),
                location);
            extendedValue = builder.makeConcat({zeros, value},
                                               static_cast<int32_t>(baseWidth), location);
        }
        ExprNodeId shiftedValue =
            builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kShl,
                                           {extendedValue, slice.indexExpr},
                                           static_cast<int32_t>(baseWidth),
                                           location);
        return builder.makeOperation(wolvrix::lib::grh::OperationKind::kOr,
                                     {masked, shiftedValue}, location);
    };

    std::function<ExprNodeId(ExprNodeId, const slang::ast::Type*, int64_t,
                             const std::vector<WriteSlice>&, std::size_t, ExprNodeId,
                             slang::SourceLocation)>
        applySlices;
    applySlices = [&](ExprNodeId base, const slang::ast::Type* baseType,
                      int64_t baseWidth, const std::vector<WriteSlice>& slices,
                      std::size_t start, ExprNodeId value,
                      slang::SourceLocation location) -> ExprNodeId {
        if (start >= slices.size())
        {
            return value;
        }
        if (baseWidth <= 0)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(location,
                                           "Write-back slice missing target width");
            }
            return kInvalidPlanIndex;
        }
        SliceSelection selection;
        if (!resolveSliceSelection(slices[start], baseType, baseWidth, selection))
        {
            return kInvalidPlanIndex;
        }

        ExprNodeId subValue = buildSliceExpr(base, selection, location);
        if (subValue == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        ExprNodeId updatedSub =
            applySlices(subValue, selection.subType, selection.subWidth, slices,
                        start + 1, value, location);
        if (updatedSub == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        if (selection.isStatic)
        {
            return spliceStatic(base, selection, updatedSub, baseWidth, location);
        }
        return spliceDynamic(base, selection, updatedSub, baseWidth, location);
    };

    auto replaceSymbolWithValue =
        [&](ExprNodeId root, PlanSymbolId target, ExprNodeId replacement) -> ExprNodeId {
        if (root == kInvalidPlanIndex || replacement == kInvalidPlanIndex)
        {
            return root;
        }
        std::unordered_map<ExprNodeId, ExprNodeId> memo;
        std::function<ExprNodeId(ExprNodeId)> visit = [&](ExprNodeId id) -> ExprNodeId {
            if (id == kInvalidPlanIndex)
            {
                return id;
            }
            if (id == replacement)
            {
                return id;
            }
            if (auto it = memo.find(id); it != memo.end())
            {
                return it->second;
            }
            if (id >= lowering.values.size())
            {
                memo[id] = id;
                return id;
            }
            const ExprNode& node = lowering.values[id];
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.index == target.index)
            {
                memo[id] = replacement;
                return replacement;
            }
            if (node.kind != ExprNodeKind::Operation)
            {
                memo[id] = id;
                return id;
            }
            bool changed = false;
            std::vector<ExprNodeId> newOperands;
            newOperands.reserve(node.operands.size());
            for (ExprNodeId operand : node.operands)
            {
                ExprNodeId next = visit(operand);
                if (next != operand)
                {
                    changed = true;
                }
                newOperands.push_back(next);
            }
            if (!changed)
            {
                memo[id] = id;
                return id;
            }
            ExprNodeId replaced =
                builder.makeOperationWithWidth(node.op, std::move(newOperands),
                                               node.widthHint, node.location);
            memo[id] = replaced;
            return replaced;
        };
        return visit(root);
    };

    auto exprUsesSymbol = [&](ExprNodeId root, PlanSymbolId target) -> bool {
        if (root == kInvalidPlanIndex || !target.valid())
        {
            return false;
        }
        if (root >= static_cast<ExprNodeId>(lowering.values.size()))
        {
            return false;
        }
        std::unordered_set<ExprNodeId> seen;
        std::vector<ExprNodeId> stack;
        stack.push_back(root);
        while (!stack.empty())
        {
            const ExprNodeId current = stack.back();
            stack.pop_back();
            if (current == kInvalidPlanIndex ||
                current >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                continue;
            }
            if (!seen.insert(current).second)
            {
                continue;
            }
            const ExprNode& node = lowering.values[current];
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.index == target.index)
            {
                return true;
            }
            if (node.kind != ExprNodeKind::Operation)
            {
                continue;
            }
            for (ExprNodeId operand : node.operands)
            {
                if (operand != kInvalidPlanIndex)
                {
                    stack.push_back(operand);
                }
            }
        }
        return false;
    };

    std::vector<ExprNodeId> combDefinitionBySymbol(plan.symbolTable.size(),
                                                   kInvalidPlanIndex);
    auto rememberCombDefinition =
        [&](PlanSymbolId symbol, ControlDomain domain, bool sliced,
            ExprNodeId value) {
        if (!symbol.valid() || symbol.index >= combDefinitionBySymbol.size() ||
            domain != ControlDomain::Combinational || sliced ||
            value == kInvalidPlanIndex)
        {
            return;
        }
        combDefinitionBySymbol[symbol.index] = value;
    };
    auto exprUsesSymbolThroughCombDefinitions =
        [&](ExprNodeId root, PlanSymbolId target) -> bool {
        if (root == kInvalidPlanIndex || !target.valid())
        {
            return false;
        }
        std::unordered_map<ExprNodeId, bool> memo;
        std::unordered_set<PlanIndex> activeSymbols;
        std::function<bool(ExprNodeId)> visit = [&](ExprNodeId id) -> bool {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return false;
            }
            if (auto it = memo.find(id); it != memo.end())
            {
                return it->second;
            }
            const ExprNode& node = lowering.values[id];
            bool result = false;
            if (node.kind == ExprNodeKind::Symbol)
            {
                if (node.symbol.index == target.index)
                {
                    result = true;
                }
                else if (node.symbol.valid() &&
                         node.symbol.index < combDefinitionBySymbol.size() &&
                         combDefinitionBySymbol[node.symbol.index] != kInvalidPlanIndex)
                {
                    if (activeSymbols.insert(node.symbol.index).second)
                    {
                        result = visit(combDefinitionBySymbol[node.symbol.index]);
                        activeSymbols.erase(node.symbol.index);
                    }
                }
            }
            else if (node.kind == ExprNodeKind::Operation)
            {
                for (ExprNodeId operand : node.operands)
                {
                    if (visit(operand))
                    {
                        result = true;
                        break;
                    }
                }
            }
            memo.emplace(id, result);
            return result;
        };
        return visit(root);
    };
    auto exprUsesAnySymbolThroughCombDefinitions =
        [&](ExprNodeId root, const std::unordered_set<PlanIndex>& targets) -> bool {
        if (root == kInvalidPlanIndex || targets.empty())
        {
            return false;
        }
        std::unordered_map<ExprNodeId, bool> memo;
        std::unordered_set<PlanIndex> activeSymbols;
        std::function<bool(ExprNodeId)> visit = [&](ExprNodeId id) -> bool {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return false;
            }
            if (auto it = memo.find(id); it != memo.end())
            {
                return it->second;
            }
            const ExprNode& node = lowering.values[id];
            bool result = false;
            if (node.kind == ExprNodeKind::Symbol)
            {
                if (node.symbol.valid() &&
                    targets.find(node.symbol.index) != targets.end())
                {
                    result = true;
                }
                else if (node.symbol.valid() &&
                         node.symbol.index < combDefinitionBySymbol.size() &&
                         combDefinitionBySymbol[node.symbol.index] != kInvalidPlanIndex)
                {
                    if (activeSymbols.insert(node.symbol.index).second)
                    {
                        result = visit(combDefinitionBySymbol[node.symbol.index]);
                        activeSymbols.erase(node.symbol.index);
                    }
                }
            }
            else if (node.kind == ExprNodeKind::Operation)
            {
                for (ExprNodeId operand : node.operands)
                {
                    if (visit(operand))
                    {
                        result = true;
                        break;
                    }
                }
            }
            memo.emplace(id, result);
            return result;
        };
        return visit(root);
    };

    auto replaceTargetStaticSlicesWithProjection =
        [&](ExprNodeId root, PlanSymbolId target, int64_t rootWidth) -> ExprNodeId {
        if (root == kInvalidPlanIndex || !target.valid() || rootWidth <= 0)
        {
            return kInvalidPlanIndex;
        }

        auto nodeWidth = [&](ExprNodeId id, int64_t fallback = 0) -> int64_t {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return fallback;
            }
            const ExprNode& node = lowering.values[id];
            if (node.widthHint > 0)
            {
                return node.widthHint;
            }
            if (node.sliceWidth > 0)
            {
                return node.sliceWidth;
            }
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.index == target.index)
            {
                return rootWidth;
            }
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.valid() && node.symbol.index < widthBySymbol.size() &&
                widthBySymbol[node.symbol.index] > 0)
            {
                return widthBySymbol[node.symbol.index];
            }
            return fallback;
        };

        std::unordered_map<ExprNodeId, bool> usesTargetMemo;
        std::unordered_set<PlanIndex> activeUsesSymbols;
        std::function<bool(ExprNodeId)> usesTarget = [&](ExprNodeId id) -> bool {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return false;
            }
            if (auto it = usesTargetMemo.find(id); it != usesTargetMemo.end())
            {
                return it->second;
            }
            const ExprNode& node = lowering.values[id];
            bool result = false;
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.index == target.index)
            {
                result = true;
            }
            else if (node.kind == ExprNodeKind::Symbol &&
                     node.symbol.valid() &&
                     node.symbol.index < combDefinitionBySymbol.size() &&
                     combDefinitionBySymbol[node.symbol.index] != kInvalidPlanIndex)
            {
                if (!activeUsesSymbols.insert(node.symbol.index).second)
                {
                    result = true;
                }
                else
                {
                    result = usesTarget(combDefinitionBySymbol[node.symbol.index]);
                    activeUsesSymbols.erase(node.symbol.index);
                }
            }
            else if (node.kind == ExprNodeKind::Operation)
            {
                for (ExprNodeId operand : node.operands)
                {
                    if (usesTarget(operand))
                    {
                        result = true;
                        break;
                    }
                }
            }
            usesTargetMemo.emplace(id, result);
            return result;
        };

        auto makeStaticSlice = [&](ExprNodeId value, int64_t low, int64_t width,
                                   int64_t valueWidth,
                                   slang::SourceLocation location) -> ExprNodeId {
            if (value == kInvalidPlanIndex || low < 0 || width <= 0 ||
                width > std::numeric_limits<int32_t>::max())
            {
                return kInvalidPlanIndex;
            }
            if (valueWidth > 0 && low == 0 && width == valueWidth)
            {
                return value;
            }
            ExprNodeId index = builder.addConstantLiteral(std::to_string(low), location);
            return builder.makeSliceDynamic(value, index,
                                            static_cast<int32_t>(width), location);
        };

        auto isTargetSymbol = [&](ExprNodeId id) -> bool {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return false;
            }
            const ExprNode& node = lowering.values[id];
            return node.kind == ExprNodeKind::Symbol &&
                   node.symbol.index == target.index;
        };

        auto staticSliceIndex = [&](const ExprNode& node) -> std::optional<int64_t> {
            if (node.kind != ExprNodeKind::Operation ||
                node.op != wolvrix::lib::grh::OperationKind::kSliceDynamic ||
                node.operands.size() < 2)
            {
                return std::nullopt;
            }
            auto index = evalConstInt(plan, lowering, node.operands[1]);
            if (!index || *index < 0)
            {
                return std::nullopt;
            }
            return index;
        };

        struct ProjectionFrame
        {
            ExprNodeId id = kInvalidPlanIndex;
            int64_t low = 0;
            int64_t width = 0;
        };
        std::vector<ProjectionFrame> activeProjection;

        std::function<ExprNodeId(ExprNodeId, int64_t, int64_t, int64_t)> projectRange;
        projectRange = [&](ExprNodeId id, int64_t low, int64_t width,
                           int64_t knownWidth) -> ExprNodeId {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()) ||
                low < 0 || width <= 0 ||
                width > std::numeric_limits<int32_t>::max())
            {
                return kInvalidPlanIndex;
            }
            const int64_t currentWidth = knownWidth > 0
                                             ? knownWidth
                                             : nodeWidth(id);
            if (currentWidth > 0 && low + width > currentWidth)
            {
                return kInvalidPlanIndex;
            }
            for (const ProjectionFrame& frame : activeProjection)
            {
                if (frame.id == id && frame.low == low && frame.width == width)
                {
                    return kInvalidPlanIndex;
                }
            }
            activeProjection.push_back(ProjectionFrame{id, low, width});
            auto finish = [&](ExprNodeId result) -> ExprNodeId {
                activeProjection.pop_back();
                return result;
            };

            const ExprNode& node = lowering.values[id];
            if (!usesTarget(id))
            {
                return finish(makeStaticSlice(id, low, width, currentWidth, node.location));
            }
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.index == target.index)
            {
                return finish(kInvalidPlanIndex);
            }
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.valid() &&
                node.symbol.index < combDefinitionBySymbol.size() &&
                combDefinitionBySymbol[node.symbol.index] != kInvalidPlanIndex)
            {
                ExprNodeId definition = combDefinitionBySymbol[node.symbol.index];
                return finish(projectRange(definition, low, width,
                                           nodeWidth(definition, currentWidth)));
            }
            if (node.kind != ExprNodeKind::Operation)
            {
                return finish(kInvalidPlanIndex);
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
                node.operands.size() >= 2)
            {
                auto index = staticSliceIndex(node);
                if (!index)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t sliceWidth = nodeWidth(id, currentWidth);
                if (sliceWidth > 0 && low + width > sliceWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                if (*index > std::numeric_limits<int64_t>::max() - low)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t projectedLow = *index + low;
                ExprNodeId base = node.operands.front();
                if (isTargetSymbol(base))
                {
                    return finish(projectRange(root, projectedLow, width, rootWidth));
                }
                const int64_t baseWidth = nodeWidth(base);
                return finish(projectRange(base, projectedLow, width, baseWidth));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceArray &&
                node.operands.size() >= 2)
            {
                auto index = evalConstInt(plan, lowering, node.operands[1]);
                const int64_t elementWidth = nodeWidth(id, currentWidth);
                if (!index || *index < 0 || elementWidth <= 0 ||
                    low + width > elementWidth ||
                    *index > std::numeric_limits<int64_t>::max() / elementWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t elementLow = *index * elementWidth;
                if (elementLow > std::numeric_limits<int64_t>::max() - low)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t projectedLow = elementLow + low;
                ExprNodeId base = node.operands.front();
                if (isTargetSymbol(base))
                {
                    return finish(projectRange(root, projectedLow, width, rootWidth));
                }
                const int64_t baseWidth = nodeWidth(base);
                return finish(projectRange(base, projectedLow, width, baseWidth));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kAssign &&
                node.operands.size() == 1)
            {
                ExprNodeId operand = node.operands.front();
                const int64_t operandWidth = nodeWidth(operand);
                if (operandWidth > 0 && low + width > operandWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(projectRange(operand, low, width, operandWidth));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kConcat)
            {
                std::vector<int64_t> operandWidths;
                operandWidths.reserve(node.operands.size());
                int64_t totalWidth = 0;
                for (ExprNodeId operand : node.operands)
                {
                    const int64_t widthHint = nodeWidth(operand);
                    if (widthHint <= 0 ||
                        widthHint > std::numeric_limits<int32_t>::max() ||
                        totalWidth > std::numeric_limits<int64_t>::max() - widthHint)
                    {
                        return finish(kInvalidPlanIndex);
                    }
                    operandWidths.push_back(widthHint);
                    totalWidth += widthHint;
                }
                if (low + width > totalWidth)
                {
                    return finish(kInvalidPlanIndex);
                }

                const int64_t high = low + width - 1;
                int64_t cursor = totalWidth;
                std::vector<ExprNodeId> parts;
                for (std::size_t i = 0; i < node.operands.size(); ++i)
                {
                    const int64_t operandWidth = operandWidths[i];
                    const int64_t operandHigh = cursor - 1;
                    const int64_t operandLow = cursor - operandWidth;
                    cursor = operandLow;
                    const int64_t overlapLow = std::max(low, operandLow);
                    const int64_t overlapHigh = std::min(high, operandHigh);
                    if (overlapLow > overlapHigh)
                    {
                        continue;
                    }
                    ExprNodeId part =
                        projectRange(node.operands[i], overlapLow - operandLow,
                                     overlapHigh - overlapLow + 1, operandWidth);
                    if (part == kInvalidPlanIndex)
                    {
                        return finish(kInvalidPlanIndex);
                    }
                    parts.push_back(part);
                }
                if (parts.empty())
                {
                    return finish(kInvalidPlanIndex);
                }
                if (parts.size() == 1)
                {
                    return finish(parts.front());
                }
                return finish(builder.makeConcat(std::move(parts),
                                                 static_cast<int32_t>(width),
                                                 node.location));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kReplicate &&
                node.operands.size() >= 2)
            {
                auto count = evalConstInt(plan, lowering, node.operands.front());
                if (!count || *count <= 0)
                {
                    return finish(kInvalidPlanIndex);
                }
                ExprNodeId data = node.operands[1];
                const int64_t dataWidth = nodeWidth(data);
                if (dataWidth <= 0 ||
                    dataWidth > std::numeric_limits<int32_t>::max() ||
                    *count > std::numeric_limits<int64_t>::max() / dataWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t totalWidth = *count * dataWidth;
                if (low + width > totalWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t high = low + width - 1;
                std::vector<ExprNodeId> parts;
                for (int64_t repeat = *count - 1; repeat >= 0; --repeat)
                {
                    const int64_t repeatLow = repeat * dataWidth;
                    const int64_t repeatHigh = repeatLow + dataWidth - 1;
                    const int64_t overlapLow = std::max(low, repeatLow);
                    const int64_t overlapHigh = std::min(high, repeatHigh);
                    if (overlapLow > overlapHigh)
                    {
                        if (repeat == 0)
                        {
                            break;
                        }
                        continue;
                    }
                    ExprNodeId part =
                        projectRange(data, overlapLow - repeatLow,
                                     overlapHigh - overlapLow + 1, dataWidth);
                    if (part == kInvalidPlanIndex)
                    {
                        return finish(kInvalidPlanIndex);
                    }
                    parts.push_back(part);
                    if (repeat == 0)
                    {
                        break;
                    }
                }
                if (parts.empty())
                {
                    return finish(kInvalidPlanIndex);
                }
                if (parts.size() == 1)
                {
                    return finish(parts.front());
                }
                return finish(builder.makeConcat(std::move(parts),
                                                 static_cast<int32_t>(width),
                                                 node.location));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kMux &&
                node.operands.size() == 3)
            {
                if (usesTarget(node.operands[0]))
                {
                    return finish(kInvalidPlanIndex);
                }
                ExprNodeId lhs = projectRange(node.operands[1], low, width,
                                              nodeWidth(node.operands[1]));
                ExprNodeId rhs = projectRange(node.operands[2], low, width,
                                              nodeWidth(node.operands[2]));
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(builder.makeOperationWithWidth(
                    wolvrix::lib::grh::OperationKind::kMux,
                    {node.operands[0], lhs, rhs},
                    static_cast<int32_t>(width), node.location));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kNot &&
                node.operands.size() == 1)
            {
                ExprNodeId operand = projectRange(node.operands.front(), low, width,
                                                  nodeWidth(node.operands.front()));
                if (operand == kInvalidPlanIndex)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(builder.makeOperationWithWidth(
                    node.op, {operand}, static_cast<int32_t>(width), node.location));
            }

            if ((node.op == wolvrix::lib::grh::OperationKind::kAnd ||
                 node.op == wolvrix::lib::grh::OperationKind::kOr ||
                 node.op == wolvrix::lib::grh::OperationKind::kXor ||
                 node.op == wolvrix::lib::grh::OperationKind::kXnor) &&
                node.operands.size() == 2)
            {
                ExprNodeId lhs = projectRange(node.operands[0], low, width,
                                              nodeWidth(node.operands[0]));
                ExprNodeId rhs = projectRange(node.operands[1], low, width,
                                              nodeWidth(node.operands[1]));
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(builder.makeOperationWithWidth(
                    node.op, {lhs, rhs}, static_cast<int32_t>(width),
                    node.location));
            }

            return finish(kInvalidPlanIndex);
        };

        std::unordered_map<ExprNodeId, ExprNodeId> replaceMemo;
        std::unordered_set<PlanIndex> activeReplaceSymbols;
        bool failed = false;
        std::function<ExprNodeId(ExprNodeId)> replace = [&](ExprNodeId id) -> ExprNodeId {
            if (failed || id == kInvalidPlanIndex)
            {
                return id;
            }
            if (auto it = replaceMemo.find(id); it != replaceMemo.end())
            {
                return it->second;
            }
            if (id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                replaceMemo[id] = id;
                return id;
            }
            const ExprNode& node = lowering.values[id];
            if (node.kind == ExprNodeKind::Symbol)
            {
                if (node.symbol.index == target.index)
                {
                    replaceMemo[id] = id;
                    return id;
                }
                if (node.symbol.valid() &&
                    node.symbol.index < combDefinitionBySymbol.size() &&
                    combDefinitionBySymbol[node.symbol.index] != kInvalidPlanIndex &&
                    usesTarget(combDefinitionBySymbol[node.symbol.index]))
                {
                    if (!activeReplaceSymbols.insert(node.symbol.index).second)
                    {
                        failed = true;
                        replaceMemo[id] = id;
                        return id;
                    }
                    ExprNodeId expanded =
                        replace(combDefinitionBySymbol[node.symbol.index]);
                    activeReplaceSymbols.erase(node.symbol.index);
                    if (expanded == kInvalidPlanIndex || usesTarget(expanded))
                    {
                        failed = true;
                        replaceMemo[id] = id;
                        return id;
                    }
                    replaceMemo[id] = expanded;
                    return expanded;
                }
                replaceMemo[id] = id;
                return id;
            }
            if (node.kind != ExprNodeKind::Operation)
            {
                replaceMemo[id] = id;
                return id;
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
                node.operands.size() >= 2 && isTargetSymbol(node.operands.front()))
            {
                auto index = staticSliceIndex(node);
                const int64_t sliceWidth = nodeWidth(id);
                if (!index || sliceWidth <= 0 ||
                    *index > std::numeric_limits<int64_t>::max() - sliceWidth ||
                    *index + sliceWidth > rootWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                ExprNodeId projected =
                    projectRange(root, *index, sliceWidth, rootWidth);
                if (projected == kInvalidPlanIndex || usesTarget(projected))
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                replaceMemo[id] = projected;
                return projected;
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
                node.operands.size() >= 2 &&
                !isTargetSymbol(node.operands.front()) &&
                usesTarget(node.operands.front()))
            {
                auto index = staticSliceIndex(node);
                const int64_t sliceWidth = nodeWidth(id);
                ExprNodeId base = node.operands.front();
                if (!index || sliceWidth <= 0 ||
                    *index > std::numeric_limits<int64_t>::max() - sliceWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                ExprNodeId projected =
                    projectRange(base, *index, sliceWidth, nodeWidth(base));
                if (projected == kInvalidPlanIndex || usesTarget(projected))
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                replaceMemo[id] = projected;
                return projected;
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceArray &&
                node.operands.size() >= 2 && isTargetSymbol(node.operands.front()))
            {
                auto index = evalConstInt(plan, lowering, node.operands[1]);
                const int64_t elementWidth = nodeWidth(id);
                if (!index || *index < 0 || elementWidth <= 0 ||
                    *index > std::numeric_limits<int64_t>::max() / elementWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                const int64_t projectedLow = *index * elementWidth;
                if (projectedLow >
                    std::numeric_limits<int64_t>::max() - elementWidth ||
                    projectedLow + elementWidth > rootWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                ExprNodeId projected =
                    projectRange(root, projectedLow, elementWidth, rootWidth);
                if (projected == kInvalidPlanIndex || usesTarget(projected))
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                replaceMemo[id] = projected;
                return projected;
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceArray &&
                node.operands.size() >= 2 &&
                !isTargetSymbol(node.operands.front()) &&
                usesTarget(node.operands.front()))
            {
                auto index = evalConstInt(plan, lowering, node.operands[1]);
                const int64_t elementWidth = nodeWidth(id);
                ExprNodeId base = node.operands.front();
                if (!index || *index < 0 || elementWidth <= 0 ||
                    *index > std::numeric_limits<int64_t>::max() / elementWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                const int64_t projectedLow = *index * elementWidth;
                if (projectedLow >
                    std::numeric_limits<int64_t>::max() - elementWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                ExprNodeId projected =
                    projectRange(base, projectedLow, elementWidth, nodeWidth(base));
                if (projected == kInvalidPlanIndex || usesTarget(projected))
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                replaceMemo[id] = projected;
                return projected;
            }

            bool changed = false;
            std::vector<ExprNodeId> newOperands;
            newOperands.reserve(node.operands.size());
            for (ExprNodeId operand : node.operands)
            {
                ExprNodeId next = replace(operand);
                if (next != operand)
                {
                    changed = true;
                }
                newOperands.push_back(next);
            }
            if (failed || !changed)
            {
                replaceMemo[id] = id;
                return id;
            }
            ExprNodeId replaced =
                builder.makeOperationWithWidth(node.op, std::move(newOperands),
                                               node.widthHint, node.location);
            replaceMemo[id] = replaced;
            return replaced;
        };

        ExprNodeId replaced = replace(root);
        if (failed)
        {
            return kInvalidPlanIndex;
        }
        return replaced;
    };

    auto replaceStaticSlicesForSymbolSet =
        [&](ExprNodeId root, const std::unordered_set<PlanIndex>& targets)
        -> ExprNodeId {
        if (root == kInvalidPlanIndex || targets.empty())
        {
            return kInvalidPlanIndex;
        }

        auto nodeWidth = [&](ExprNodeId id, int64_t fallback = 0) -> int64_t {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return fallback;
            }
            const ExprNode& node = lowering.values[id];
            if (node.widthHint > 0)
            {
                return node.widthHint;
            }
            if (node.sliceWidth > 0)
            {
                return node.sliceWidth;
            }
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.valid() && node.symbol.index < widthBySymbol.size() &&
                widthBySymbol[node.symbol.index] > 0)
            {
                return widthBySymbol[node.symbol.index];
            }
            return fallback;
        };

        auto makeStaticSlice = [&](ExprNodeId value, int64_t low, int64_t width,
                                   int64_t valueWidth,
                                   slang::SourceLocation location) -> ExprNodeId {
            if (value == kInvalidPlanIndex || low < 0 || width <= 0 ||
                width > std::numeric_limits<int32_t>::max())
            {
                return kInvalidPlanIndex;
            }
            if (valueWidth > 0 && low == 0 && width == valueWidth)
            {
                return value;
            }
            ExprNodeId index = builder.addConstantLiteral(std::to_string(low), location);
            return builder.makeSliceDynamic(value, index,
                                            static_cast<int32_t>(width), location);
        };

        auto staticSliceIndex = [&](const ExprNode& node) -> std::optional<int64_t> {
            if (node.kind != ExprNodeKind::Operation ||
                node.op != wolvrix::lib::grh::OperationKind::kSliceDynamic ||
                node.operands.size() < 2)
            {
                return std::nullopt;
            }
            auto index = evalConstInt(plan, lowering, node.operands[1]);
            if (!index || *index < 0)
            {
                return std::nullopt;
            }
            return index;
        };

        auto isTargetSymbol = [&](ExprNodeId id) -> std::optional<PlanSymbolId> {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return std::nullopt;
            }
            const ExprNode& node = lowering.values[id];
            if (node.kind != ExprNodeKind::Symbol || !node.symbol.valid() ||
                targets.find(node.symbol.index) == targets.end())
            {
                return std::nullopt;
            }
            return node.symbol;
        };

        std::unordered_map<ExprNodeId, bool> usesTargetMemo;
        std::unordered_set<PlanIndex> activeUsesSymbols;
        std::function<bool(ExprNodeId)> usesTargets = [&](ExprNodeId id) -> bool {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return false;
            }
            if (auto it = usesTargetMemo.find(id); it != usesTargetMemo.end())
            {
                return it->second;
            }
            const ExprNode& node = lowering.values[id];
            bool result = false;
            if (node.kind == ExprNodeKind::Symbol)
            {
                if (node.symbol.valid() &&
                    targets.find(node.symbol.index) != targets.end())
                {
                    result = true;
                }
                else if (node.symbol.valid() &&
                         node.symbol.index < combDefinitionBySymbol.size() &&
                         combDefinitionBySymbol[node.symbol.index] != kInvalidPlanIndex)
                {
                    if (!activeUsesSymbols.insert(node.symbol.index).second)
                    {
                        result = true;
                    }
                    else
                    {
                        result = usesTargets(combDefinitionBySymbol[node.symbol.index]);
                        activeUsesSymbols.erase(node.symbol.index);
                    }
                }
            }
            else if (node.kind == ExprNodeKind::Operation)
            {
                for (ExprNodeId operand : node.operands)
                {
                    if (usesTargets(operand))
                    {
                        result = true;
                        break;
                    }
                }
            }
            usesTargetMemo.emplace(id, result);
            return result;
        };

        struct ProjectionFrame
        {
            ExprNodeId id = kInvalidPlanIndex;
            int64_t low = 0;
            int64_t width = 0;
        };
        std::vector<ProjectionFrame> activeProjection;

        std::function<ExprNodeId(ExprNodeId, int64_t, int64_t, int64_t)> projectRange;
        projectRange = [&](ExprNodeId id, int64_t low, int64_t width,
                           int64_t knownWidth) -> ExprNodeId {
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()) ||
                low < 0 || width <= 0 ||
                width > std::numeric_limits<int32_t>::max())
            {
                return kInvalidPlanIndex;
            }
            const int64_t currentWidth = knownWidth > 0
                                             ? knownWidth
                                             : nodeWidth(id);
            if (currentWidth > 0 && low + width > currentWidth)
            {
                return kInvalidPlanIndex;
            }
            for (const ProjectionFrame& frame : activeProjection)
            {
                if (frame.id == id && frame.low == low && frame.width == width)
                {
                    return kInvalidPlanIndex;
                }
            }
            activeProjection.push_back(ProjectionFrame{id, low, width});
            auto finish = [&](ExprNodeId result) -> ExprNodeId {
                activeProjection.pop_back();
                return result;
            };

            const ExprNode& node = lowering.values[id];
            if (!usesTargets(id))
            {
                return finish(makeStaticSlice(id, low, width, currentWidth, node.location));
            }
            if (auto symbol = isTargetSymbol(id))
            {
                if (symbol->index >= combDefinitionBySymbol.size() ||
                    combDefinitionBySymbol[symbol->index] == kInvalidPlanIndex)
                {
                    return finish(kInvalidPlanIndex);
                }
                ExprNodeId definition = combDefinitionBySymbol[symbol->index];
                return finish(projectRange(definition, low, width,
                                           nodeWidth(definition, currentWidth)));
            }
            if (node.kind == ExprNodeKind::Symbol &&
                node.symbol.valid() &&
                node.symbol.index < combDefinitionBySymbol.size() &&
                combDefinitionBySymbol[node.symbol.index] != kInvalidPlanIndex)
            {
                ExprNodeId definition = combDefinitionBySymbol[node.symbol.index];
                return finish(projectRange(definition, low, width,
                                           nodeWidth(definition, currentWidth)));
            }
            if (node.kind != ExprNodeKind::Operation)
            {
                return finish(kInvalidPlanIndex);
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
                node.operands.size() >= 2)
            {
                auto index = staticSliceIndex(node);
                if (!index)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t sliceWidth = nodeWidth(id, currentWidth);
                if (sliceWidth > 0 && low + width > sliceWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                if (*index > std::numeric_limits<int64_t>::max() - low)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t projectedLow = *index + low;
                ExprNodeId base = node.operands.front();
                return finish(projectRange(base, projectedLow, width, nodeWidth(base)));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceArray &&
                node.operands.size() >= 2)
            {
                auto index = evalConstInt(plan, lowering, node.operands[1]);
                const int64_t elementWidth = nodeWidth(id, currentWidth);
                if (!index || *index < 0 || elementWidth <= 0 ||
                    low + width > elementWidth ||
                    *index > std::numeric_limits<int64_t>::max() / elementWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t elementLow = *index * elementWidth;
                if (elementLow > std::numeric_limits<int64_t>::max() - low)
                {
                    return finish(kInvalidPlanIndex);
                }
                ExprNodeId base = node.operands.front();
                return finish(projectRange(base, elementLow + low, width,
                                           nodeWidth(base)));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kAssign &&
                node.operands.size() == 1)
            {
                ExprNodeId operand = node.operands.front();
                const int64_t operandWidth = nodeWidth(operand);
                if (operandWidth > 0 && low + width > operandWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(projectRange(operand, low, width, operandWidth));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kConcat)
            {
                std::vector<int64_t> operandWidths;
                operandWidths.reserve(node.operands.size());
                int64_t totalWidth = 0;
                for (ExprNodeId operand : node.operands)
                {
                    const int64_t operandWidth = nodeWidth(operand);
                    if (operandWidth <= 0 ||
                        operandWidth > std::numeric_limits<int32_t>::max() ||
                        totalWidth > std::numeric_limits<int64_t>::max() - operandWidth)
                    {
                        return finish(kInvalidPlanIndex);
                    }
                    operandWidths.push_back(operandWidth);
                    totalWidth += operandWidth;
                }
                if (low + width > totalWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t high = low + width - 1;
                int64_t cursor = totalWidth;
                std::vector<ExprNodeId> parts;
                for (std::size_t i = 0; i < node.operands.size(); ++i)
                {
                    const int64_t operandWidth = operandWidths[i];
                    const int64_t operandHigh = cursor - 1;
                    const int64_t operandLow = cursor - operandWidth;
                    cursor = operandLow;
                    const int64_t overlapLow = std::max(low, operandLow);
                    const int64_t overlapHigh = std::min(high, operandHigh);
                    if (overlapLow > overlapHigh)
                    {
                        continue;
                    }
                    ExprNodeId part =
                        projectRange(node.operands[i], overlapLow - operandLow,
                                     overlapHigh - overlapLow + 1, operandWidth);
                    if (part == kInvalidPlanIndex)
                    {
                        return finish(kInvalidPlanIndex);
                    }
                    parts.push_back(part);
                }
                if (parts.empty())
                {
                    return finish(kInvalidPlanIndex);
                }
                if (parts.size() == 1)
                {
                    return finish(parts.front());
                }
                return finish(builder.makeConcat(std::move(parts),
                                                 static_cast<int32_t>(width),
                                                 node.location));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kReplicate &&
                node.operands.size() >= 2)
            {
                auto count = evalConstInt(plan, lowering, node.operands.front());
                if (!count || *count <= 0)
                {
                    return finish(kInvalidPlanIndex);
                }
                ExprNodeId data = node.operands[1];
                const int64_t dataWidth = nodeWidth(data);
                if (dataWidth <= 0 ||
                    dataWidth > std::numeric_limits<int32_t>::max() ||
                    *count > std::numeric_limits<int64_t>::max() / dataWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t totalWidth = *count * dataWidth;
                if (low + width > totalWidth)
                {
                    return finish(kInvalidPlanIndex);
                }
                const int64_t high = low + width - 1;
                std::vector<ExprNodeId> parts;
                for (int64_t repeat = *count - 1; repeat >= 0; --repeat)
                {
                    const int64_t repeatLow = repeat * dataWidth;
                    const int64_t repeatHigh = repeatLow + dataWidth - 1;
                    const int64_t overlapLow = std::max(low, repeatLow);
                    const int64_t overlapHigh = std::min(high, repeatHigh);
                    if (overlapLow > overlapHigh)
                    {
                        if (repeat == 0)
                        {
                            break;
                        }
                        continue;
                    }
                    ExprNodeId part =
                        projectRange(data, overlapLow - repeatLow,
                                     overlapHigh - overlapLow + 1, dataWidth);
                    if (part == kInvalidPlanIndex)
                    {
                        return finish(kInvalidPlanIndex);
                    }
                    parts.push_back(part);
                    if (repeat == 0)
                    {
                        break;
                    }
                }
                if (parts.empty())
                {
                    return finish(kInvalidPlanIndex);
                }
                if (parts.size() == 1)
                {
                    return finish(parts.front());
                }
                return finish(builder.makeConcat(std::move(parts),
                                                 static_cast<int32_t>(width),
                                                 node.location));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kMux &&
                node.operands.size() == 3)
            {
                if (usesTargets(node.operands[0]))
                {
                    return finish(kInvalidPlanIndex);
                }
                ExprNodeId lhs = projectRange(node.operands[1], low, width,
                                              nodeWidth(node.operands[1]));
                ExprNodeId rhs = projectRange(node.operands[2], low, width,
                                              nodeWidth(node.operands[2]));
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(builder.makeOperationWithWidth(
                    wolvrix::lib::grh::OperationKind::kMux,
                    {node.operands[0], lhs, rhs},
                    static_cast<int32_t>(width), node.location));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kNot &&
                node.operands.size() == 1)
            {
                ExprNodeId operand = projectRange(node.operands.front(), low, width,
                                                  nodeWidth(node.operands.front()));
                if (operand == kInvalidPlanIndex)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(builder.makeOperationWithWidth(
                    node.op, {operand}, static_cast<int32_t>(width), node.location));
            }

            if ((node.op == wolvrix::lib::grh::OperationKind::kAnd ||
                 node.op == wolvrix::lib::grh::OperationKind::kOr ||
                 node.op == wolvrix::lib::grh::OperationKind::kXor ||
                 node.op == wolvrix::lib::grh::OperationKind::kXnor) &&
                node.operands.size() == 2)
            {
                ExprNodeId lhs = projectRange(node.operands[0], low, width,
                                              nodeWidth(node.operands[0]));
                ExprNodeId rhs = projectRange(node.operands[1], low, width,
                                              nodeWidth(node.operands[1]));
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return finish(kInvalidPlanIndex);
                }
                return finish(builder.makeOperationWithWidth(
                    node.op, {lhs, rhs}, static_cast<int32_t>(width),
                    node.location));
            }

            return finish(kInvalidPlanIndex);
        };

        std::unordered_map<ExprNodeId, ExprNodeId> replaceMemo;
        std::unordered_set<PlanIndex> activeReplaceSymbols;
        bool failed = false;
        std::function<ExprNodeId(ExprNodeId)> replace = [&](ExprNodeId id) -> ExprNodeId {
            if (failed || id == kInvalidPlanIndex)
            {
                return id;
            }
            if (auto it = replaceMemo.find(id); it != replaceMemo.end())
            {
                return it->second;
            }
            if (id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                replaceMemo[id] = id;
                return id;
            }
            const ExprNode& node = lowering.values[id];
            if (node.kind == ExprNodeKind::Symbol)
            {
                if (node.symbol.valid() &&
                    targets.find(node.symbol.index) != targets.end())
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                replaceMemo[id] = id;
                return id;
            }
            if (node.kind != ExprNodeKind::Operation)
            {
                replaceMemo[id] = id;
                return id;
            }

            auto replaceProjection = [&](ExprNodeId projected) -> ExprNodeId {
                if (projected == kInvalidPlanIndex || usesTargets(projected))
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                replaceMemo[id] = projected;
                return projected;
            };

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
                node.operands.size() >= 2 && usesTargets(node.operands.front()))
            {
                auto index = staticSliceIndex(node);
                const int64_t sliceWidth = nodeWidth(id);
                if (!index || sliceWidth <= 0 ||
                    *index > std::numeric_limits<int64_t>::max() - sliceWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                return replaceProjection(
                    projectRange(node.operands.front(), *index, sliceWidth,
                                 nodeWidth(node.operands.front())));
            }

            if (node.op == wolvrix::lib::grh::OperationKind::kSliceArray &&
                node.operands.size() >= 2 && usesTargets(node.operands.front()))
            {
                auto index = evalConstInt(plan, lowering, node.operands[1]);
                const int64_t elementWidth = nodeWidth(id);
                if (!index || *index < 0 || elementWidth <= 0 ||
                    *index > std::numeric_limits<int64_t>::max() / elementWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                const int64_t projectedLow = *index * elementWidth;
                if (projectedLow >
                    std::numeric_limits<int64_t>::max() - elementWidth)
                {
                    failed = true;
                    replaceMemo[id] = id;
                    return id;
                }
                return replaceProjection(
                    projectRange(node.operands.front(), projectedLow, elementWidth,
                                 nodeWidth(node.operands.front())));
            }

            bool changed = false;
            std::vector<ExprNodeId> newOperands;
            newOperands.reserve(node.operands.size());
            for (ExprNodeId operand : node.operands)
            {
                ExprNodeId next = replace(operand);
                if (next != operand)
                {
                    changed = true;
                }
                newOperands.push_back(next);
            }
            if (failed || !changed)
            {
                replaceMemo[id] = id;
                return id;
            }
            ExprNodeId replaced =
                builder.makeOperationWithWidth(node.op, std::move(newOperands),
                                               node.widthHint, node.location);
            replaceMemo[id] = replaced;
            return replaced;
        };

        ExprNodeId replaced = replace(root);
        if (failed)
        {
            return kInvalidPlanIndex;
        }
        return replaced;
    };

    auto slicesCoverFullWidth =
        [&](const std::vector<const LoweredStmt*>& writes, int64_t baseWidth,
            const slang::ast::Type* baseType, PlanSymbolId target) -> bool {
        if (baseWidth <= 0)
        {
            return false;
        }
        std::vector<std::pair<int64_t, int64_t>> ranges;
        ranges.reserve(writes.size());
        for (const LoweredStmt* stmt : writes)
        {
            const WriteIntent& write = stmt->write;
            if (write.slices.empty())
            {
                ranges.clear();
                ranges.emplace_back(0, baseWidth - 1);
                continue;
            }
            SliceSelection selection;
            if (!resolveSliceSelection(write.slices.front(), baseType, baseWidth,
                                       selection) ||
                !selection.isStatic || selection.width <= 0)
            {
                return false;
            }
            const int64_t low = selection.low;
            const int64_t high = selection.low + selection.width - 1;
            ranges.emplace_back(low, high);
        }
        if (ranges.empty())
        {
            return false;
        }
        std::sort(ranges.begin(), ranges.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs.first != rhs.first)
                      {
                          return lhs.first < rhs.first;
                      }
                      return lhs.second < rhs.second;
                  });
        int64_t coveredLow = ranges.front().first;
        int64_t coveredHigh = ranges.front().second;
        if (coveredLow > 0)
        {
            return false;
        }
        for (std::size_t i = 1; i < ranges.size(); ++i)
        {
            if (ranges[i].first > coveredHigh + 1)
            {
                return false;
            }
            if (ranges[i].second > coveredHigh)
            {
                coveredHigh = ranges[i].second;
                if (coveredHigh >= baseWidth - 1)
                {
                    return true;
                }
            }
        }
        return coveredHigh >= baseWidth - 1;
    };

    auto isLogicNotOfExpr = [&](ExprNodeId maybeNot, ExprNodeId operand) -> bool {
        if (maybeNot == kInvalidPlanIndex || operand == kInvalidPlanIndex)
        {
            return false;
        }
        if (maybeNot >= static_cast<ExprNodeId>(lowering.values.size()))
        {
            return false;
        }
        const ExprNode& node = lowering.values[maybeNot];
        if (node.kind != ExprNodeKind::Operation)
        {
            return false;
        }
        if (node.op != wolvrix::lib::grh::OperationKind::kLogicNot &&
            node.op != wolvrix::lib::grh::OperationKind::kNot)
        {
            return false;
        }
        return !node.operands.empty() && node.operands.front() == operand;
    };

    auto isTautologyByDnf = [&](ExprNodeId id) -> bool {
        if (id == kInvalidPlanIndex)
        {
            return false;
        }
        std::vector<ExprNodeId> leaves;
        std::unordered_map<ExprNodeId, std::size_t> leafIndex;

        std::function<void(ExprNodeId)> collectLeaves = [&](ExprNodeId nodeId) {
            if (nodeId == kInvalidPlanIndex)
            {
                return;
            }
            if (auto literal = evalConstInt(plan, lowering, nodeId))
            {
                (void)literal;
                return;
            }
            if (nodeId >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                if (leafIndex.find(nodeId) == leafIndex.end())
                {
                    leafIndex[nodeId] = leaves.size();
                    leaves.push_back(nodeId);
                }
                return;
            }
            const ExprNode& node = lowering.values[nodeId];
            if (node.kind == ExprNodeKind::Operation)
            {
                if (node.op == wolvrix::lib::grh::OperationKind::kLogicAnd ||
                    node.op == wolvrix::lib::grh::OperationKind::kLogicOr ||
                    node.op == wolvrix::lib::grh::OperationKind::kLogicNot ||
                    node.op == wolvrix::lib::grh::OperationKind::kNot)
                {
                    for (ExprNodeId operand : node.operands)
                    {
                        collectLeaves(operand);
                    }
                    return;
                }
            }
            if (leafIndex.find(nodeId) == leafIndex.end())
            {
                leafIndex[nodeId] = leaves.size();
                leaves.push_back(nodeId);
            }
        };
        collectLeaves(id);
        if (leaves.size() > 8)
        {
            return false;
        }

        auto evalExpr = [&](ExprNodeId nodeId,
                            const std::vector<bool>& assignment,
                            auto&& self) -> bool {
            if (nodeId == kInvalidPlanIndex)
            {
                return false;
            }
            if (auto literal = evalConstInt(plan, lowering, nodeId))
            {
                return *literal != 0;
            }
            if (nodeId >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                auto it = leafIndex.find(nodeId);
                return it != leafIndex.end() && assignment[it->second];
            }
            const ExprNode& node = lowering.values[nodeId];
            if (node.kind == ExprNodeKind::Operation)
            {
                if (node.op == wolvrix::lib::grh::OperationKind::kLogicAnd)
                {
                    for (ExprNodeId operand : node.operands)
                    {
                        if (!self(operand, assignment, self))
                        {
                            return false;
                        }
                    }
                    return true;
                }
                if (node.op == wolvrix::lib::grh::OperationKind::kLogicOr)
                {
                    for (ExprNodeId operand : node.operands)
                    {
                        if (self(operand, assignment, self))
                        {
                            return true;
                        }
                    }
                    return false;
                }
                if (node.op == wolvrix::lib::grh::OperationKind::kLogicNot ||
                    node.op == wolvrix::lib::grh::OperationKind::kNot)
                {
                    if (node.operands.empty())
                    {
                        return false;
                    }
                    return !self(node.operands.front(), assignment, self);
                }
            }
            auto it = leafIndex.find(nodeId);
            return it != leafIndex.end() && assignment[it->second];
        };

        const std::size_t total =
            leaves.empty() ? 1u : (static_cast<std::size_t>(1) << leaves.size());
        std::vector<bool> assignment(leaves.size(), false);
        for (std::size_t mask = 0; mask < total; ++mask)
        {
            for (std::size_t i = 0; i < leaves.size(); ++i)
            {
                assignment[i] = ((mask >> i) & 1U) != 0U;
            }
            if (!evalExpr(id, assignment, evalExpr))
            {
                return false;
            }
        }
        return true;
    };

    std::function<bool(ExprNodeId)> isAlwaysTrueExpr;
    isAlwaysTrueExpr = [&](ExprNodeId id) -> bool {
        if (id == kInvalidPlanIndex)
        {
            return false;
        }
        if (auto literal = evalConstInt(plan, lowering, id))
        {
            return *literal != 0;
        }
        if (id >= static_cast<ExprNodeId>(lowering.values.size()))
        {
            return false;
        }
        const ExprNode& node = lowering.values[id];
        if (node.kind != ExprNodeKind::Operation ||
            node.op != wolvrix::lib::grh::OperationKind::kLogicOr)
        {
            return false;
        }
        for (std::size_t i = 0; i < node.operands.size(); ++i)
        {
            const ExprNodeId lhs = node.operands[i];
            if (isAlwaysTrueExpr(lhs))
            {
                return true;
            }
            for (std::size_t j = i + 1; j < node.operands.size(); ++j)
            {
                const ExprNodeId rhs = node.operands[j];
                if (isLogicNotOfExpr(lhs, rhs) || isLogicNotOfExpr(rhs, lhs))
                {
                    return true;
                }
            }
        }
        return isTautologyByDnf(id);
    };

    auto finalizeDomain =
        [&](ControlDomain domain, bool hasUnconditional, ExprNodeId updateCond,
            bool usesTarget) -> ControlDomain {
        if (domain == ControlDomain::Combinational && !hasUnconditional)
        {
            const bool alwaysTrue = isAlwaysTrueExpr(updateCond);
            if (!alwaysTrue || usesTarget)
            {
                return ControlDomain::Latch;
            }
        }
        return domain;
    };

    for (const auto& group : groups)
    {
        if (group.writes.empty())
        {
            continue;
        }

        WriteBackPlan::Entry entry;
        entry.target = group.target;
        if (entry.target.valid() && entry.target.index < signalBySymbol.size())
        {
            entry.signal = signalBySymbol[entry.target.index];
        }
        entry.domain = group.domain;
        entry.eventEdges = group.eventEdges;
        entry.eventOperands = group.eventOperands;
        entry.location = group.writes.front()->location;

        bool hasSlices = false;
        bool hasUnconditional = false;
        bool allCoverAllTwoState = true;
        for (const LoweredStmt* stmt : group.writes)
        {
            const WriteIntent& write = stmt->write;
            if (!write.slices.empty())
            {
                hasSlices = true;
            }
            if (!write.coversAllTwoState)
            {
                allCoverAllTwoState = false;
            }
            if (write.coversAllTwoState && write.slices.empty())
            {
                hasUnconditional = true;
                continue;
            }
            if (write.guard == kInvalidPlanIndex)
            {
                hasUnconditional = true;
                continue;
            }
            if (auto guardConst = evalConstInt(plan, lowering, write.guard))
            {
                if (*guardConst != 0)
                {
                    hasUnconditional = true;
                }
            }
        }

        const int64_t baseWidth =
            entry.target.valid() && entry.target.index < widthBySymbol.size()
                ? widthBySymbol[entry.target.index]
                : 0;
        const slang::ast::Type* baseType =
            entry.target.valid() && entry.target.index < typeBySymbol.size()
                ? typeBySymbol[entry.target.index]
                : nullptr;
        const bool baseIsSigned = baseType ? baseType->isSigned() : false;
        const int32_t baseWidthHint =
            baseWidth > 0
                ? static_cast<int32_t>(std::min<int64_t>(
                      baseWidth, std::numeric_limits<int32_t>::max()))
                : 0;
        auto normalizeWriteBackValue = [&](ExprNodeId value, slang::SourceLocation location) {
            if (value == kInvalidPlanIndex || baseWidthHint <= 0)
            {
                return value;
            }
            return builder.resizeValueToWidth(value, baseWidthHint, baseIsSigned, location);
        };
        bool fullWidthStaticSlice = false;
        if (!group.writes.empty() && baseWidth > 0)
        {
            bool sliceOk = true;
            bool hasRef = false;
            SliceSelection ref{};
            for (const LoweredStmt* stmt : group.writes)
            {
                const WriteIntent& write = stmt->write;
                if (write.slices.size() != 1)
                {
                    sliceOk = false;
                    break;
                }
                SliceSelection selection;
                if (!resolveSliceSelection(write.slices.front(), baseType, baseWidth,
                                           selection) ||
                    !selection.isStatic || selection.width <= 0)
                {
                    sliceOk = false;
                    break;
                }
                if (!hasRef)
                {
                    ref = selection;
                    hasRef = true;
                    continue;
                }
                if (selection.low != ref.low || selection.width != ref.width)
                {
                    sliceOk = false;
                    break;
                }
            }
            if (sliceOk && hasRef)
            {
                entry.hasStaticSlice = true;
                entry.sliceLow = ref.low;
                entry.sliceWidth = ref.width;
            }
        }
        if (entry.hasStaticSlice && baseWidth > 0 &&
            entry.sliceLow == 0 && entry.sliceWidth == baseWidth)
        {
            fullWidthStaticSlice = true;
            if (allCoverAllTwoState)
            {
                hasUnconditional = true;
            }
        }
        bool zeroBaseForSlices = false;
        if (hasSlices && baseWidth > 0)
        {
            bool allUnconditional = true;
            for (const LoweredStmt* stmt : group.writes)
            {
                const WriteIntent& write = stmt->write;
                if (write.slices.empty())
                {
                    continue;
                }
                bool guardAlwaysTrue = write.guard == kInvalidPlanIndex;
                if (!guardAlwaysTrue)
                {
                    if (auto guardConst = evalConstInt(plan, lowering, write.guard))
                    {
                        guardAlwaysTrue = (*guardConst != 0);
                    }
                }
                if (!guardAlwaysTrue)
                {
                    allUnconditional = false;
                    break;
                }
            }
            const bool slicesCoverFull =
                slicesCoverFullWidth(group.writes, baseWidth, baseType, entry.target);
            // For combinational domain, if slices cover the full width,
            // use zero base to avoid feedback loop even if writes are conditional.
            // This is safe because combinational always blocks should fully
            // assign the target without using its previous value.
            const bool fullCoverage =
                allUnconditional && slicesCoverFull;
            const bool combinationalFullCoverage =
                (entry.domain == ControlDomain::Combinational) && slicesCoverFull;
            zeroBaseForSlices = fullCoverage || combinationalFullCoverage;
        }
        if (hasSlices && fullWidthStaticSlice)
        {
            zeroBaseForSlices = true;
        }

        ExprNodeId directConcatValue = kInvalidPlanIndex;
        if (hasSlices && zeroBaseForSlices)
        {
            struct SliceAssignment
            {
                int64_t low = 0;
                int64_t width = 0;
                ExprNodeId value = kInvalidPlanIndex;
            };
            std::vector<SliceAssignment> assignments;
            assignments.reserve(group.writes.size());
            bool canConcat = true;
            for (const LoweredStmt* stmt : group.writes)
            {
                const WriteIntent& write = stmt->write;
                if (write.slices.size() != 1)
                {
                    canConcat = false;
                    break;
                }
                SliceSelection selection;
                if (!resolveSliceSelection(write.slices.front(), baseType, baseWidth,
                                           selection) ||
                    !selection.isStatic || selection.width <= 0)
                {
                    canConcat = false;
                    break;
                }
                assignments.push_back(
                    SliceAssignment{selection.low, selection.width, write.value});
            }
            if (canConcat)
            {
                std::sort(assignments.begin(), assignments.end(),
                          [](const SliceAssignment& lhs, const SliceAssignment& rhs) {
                              return lhs.low < rhs.low;
                          });
                int64_t expectedLow = 0;
                for (const auto& assignment : assignments)
                {
                    if (assignment.low != expectedLow || assignment.width <= 0)
                    {
                        canConcat = false;
                        break;
                    }
                    expectedLow = assignment.low + assignment.width;
                }
                if (canConcat && expectedLow == baseWidth)
                {
                    std::sort(assignments.begin(), assignments.end(),
                              [](const SliceAssignment& lhs, const SliceAssignment& rhs) {
                                  return lhs.low > rhs.low;
                              });
                    std::vector<ExprNodeId> operands;
                    operands.reserve(assignments.size());
                    for (const auto& assignment : assignments)
                    {
                        operands.push_back(assignment.value);
                    }
                    const int32_t widthHint =
                        static_cast<int32_t>(std::min<int64_t>(
                            baseWidth, std::numeric_limits<int32_t>::max()));
                    directConcatValue =
                        builder.makeConcat(std::move(operands), widthHint,
                                           entry.location);
                }
            }
        }

        ExprNodeId updateCond = kInvalidPlanIndex;
        ExprNodeId baseValue = kInvalidPlanIndex;
        if (hasSlices)
        {
            if (zeroBaseForSlices)
            {
                const uint64_t maxValue =
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                const int32_t widthHint =
                    baseWidth > 0
                        ? static_cast<int32_t>(
                              std::min<int64_t>(baseWidth, maxValue))
                        : 0;
                baseValue = builder.addConstantLiteralWithWidth("0", widthHint,
                                                                entry.location);
            }
            else
            {
                baseValue = builder.addSymbol(group.target, entry.location);
            }
        }
        ExprNodeId nextValue = hasSlices ? baseValue : kInvalidPlanIndex;

        bool allGuardsTrue = true;
        if (directConcatValue != kInvalidPlanIndex)
        {
            for (const LoweredStmt* stmt : group.writes)
            {
                const WriteIntent& write = stmt->write;
                bool guardAlwaysTrue = write.guard == kInvalidPlanIndex;
                if (!guardAlwaysTrue)
                {
                    if (auto guardConst = evalConstInt(plan, lowering, write.guard))
                    {
                        guardAlwaysTrue = (*guardConst != 0);
                    }
                }
                if (!guardAlwaysTrue)
                {
                    allGuardsTrue = false;
                    break;
                }
            }
        }

        if (directConcatValue != kInvalidPlanIndex && allGuardsTrue)
        {
            directConcatValue = normalizeWriteBackValue(directConcatValue, entry.location);
            if (directConcatValue == kInvalidPlanIndex)
            {
                continue;
            }
            entry.updateCond =
                builder.ensureGuardExpr(kInvalidPlanIndex, entry.location);
            entry.nextValue = directConcatValue;
            const bool usesTarget =
                exprUsesSymbolThroughCombDefinitions(entry.nextValue, entry.target);
            entry.domain = finalizeDomain(entry.domain, hasUnconditional, entry.updateCond,
                                          usesTarget);
            rememberCombDefinition(entry.target, entry.domain, hasSlices, entry.nextValue);
            resultEntryHasSlices.push_back(hasSlices ? 1 : 0);
            resultEntryOriginalDomain.push_back(group.domain);
            result.entries.push_back(std::move(entry));
            continue;
        }

        const ControlDomain domain = entry.domain;
        for (const LoweredStmt* stmt : group.writes)
        {
            const WriteIntent& write = stmt->write;
            ExprNodeId guard = builder.ensureGuardExpr(write.guard, write.location);
            bool guardAlwaysTrue = write.guard == kInvalidPlanIndex;
            if (!guardAlwaysTrue)
            {
                if (auto guardConst = evalConstInt(plan, lowering, write.guard))
                {
                    guardAlwaysTrue = (*guardConst != 0);
                }
            }
            ExprNodeId writeValue = write.value;
            if (!write.slices.empty())
            {
                writeValue = applySlices(nextValue, baseType, baseWidth,
                                         write.slices, 0, write.value, write.location);
                if (writeValue == kInvalidPlanIndex)
                {
                    continue;
                }
            }
            if (!write.isNonBlocking && domain != ControlDomain::Sequential &&
                nextValue != kInvalidPlanIndex)
            {
                writeValue =
                    replaceSymbolWithValue(writeValue, group.target, nextValue);
            }
            updateCond = builder.makeLogicOr(updateCond, guard, write.location);
            if (guardAlwaysTrue)
            {
                nextValue = writeValue;
                continue;
            }
            if (nextValue == kInvalidPlanIndex)
            {
                nextValue = writeValue;
                continue;
            }
            nextValue = builder.makeMux(guard, writeValue, nextValue, write.location);
        }

        if (updateCond == kInvalidPlanIndex || nextValue == kInvalidPlanIndex)
        {
            continue;
        }

        nextValue = normalizeWriteBackValue(nextValue, entry.location);
        if (nextValue == kInvalidPlanIndex)
        {
            continue;
        }

        const bool usesTarget =
            exprUsesSymbolThroughCombDefinitions(nextValue, entry.target);
        entry.domain = finalizeDomain(entry.domain, hasUnconditional, updateCond, usesTarget);
        entry.updateCond = updateCond;
        entry.nextValue = nextValue;
        rememberCombDefinition(entry.target, entry.domain, hasSlices, entry.nextValue);
        resultEntryHasSlices.push_back(hasSlices ? 1 : 0);
        resultEntryOriginalDomain.push_back(group.domain);
        result.entries.push_back(std::move(entry));
    }

    auto rebuildCombDefinitionMap = [&](bool includeOriginalComb) {
        if (combDefinitionBySymbol.size() < plan.symbolTable.size())
        {
            combDefinitionBySymbol.resize(plan.symbolTable.size(), kInvalidPlanIndex);
        }
        std::fill(combDefinitionBySymbol.begin(), combDefinitionBySymbol.end(),
                  kInvalidPlanIndex);
        for (std::size_t i = 0; i < result.entries.size(); ++i)
        {
            const WriteBackPlan::Entry& entry = result.entries[i];
            const bool hasSlices =
                i < resultEntryHasSlices.size() && resultEntryHasSlices[i] != 0;
            const ControlDomain mapDomain =
                includeOriginalComb && i < resultEntryOriginalDomain.size()
                    ? resultEntryOriginalDomain[i]
                    : entry.domain;
            rememberCombDefinition(entry.target, mapDomain, hasSlices,
                                   entry.nextValue);
        }
    };
    auto collectDirectSymbols = [&](ExprNodeId root) {
        std::unordered_set<PlanIndex> symbols;
        if (root == kInvalidPlanIndex)
        {
            return symbols;
        }
        std::unordered_set<ExprNodeId> seen;
        std::vector<ExprNodeId> stack;
        stack.push_back(root);
        while (!stack.empty())
        {
            ExprNodeId id = stack.back();
            stack.pop_back();
            if (id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                continue;
            }
            if (!seen.insert(id).second)
            {
                continue;
            }
            const ExprNode& node = lowering.values[id];
            if (node.kind == ExprNodeKind::Symbol)
            {
                if (node.symbol.valid())
                {
                    symbols.insert(node.symbol.index);
                }
                continue;
            }
            if (node.kind != ExprNodeKind::Operation)
            {
                continue;
            }
            for (ExprNodeId operand : node.operands)
            {
                stack.push_back(operand);
            }
        }
        return symbols;
    };
    auto projectCombSymbolSccs = [&]() -> bool {
        std::unordered_map<PlanIndex, std::size_t> entryBySymbol;
        for (std::size_t i = 0; i < result.entries.size(); ++i)
        {
            const WriteBackPlan::Entry& entry = result.entries[i];
            const bool hasSlices =
                i < resultEntryHasSlices.size() && resultEntryHasSlices[i] != 0;
            const ControlDomain originalDomain =
                i < resultEntryOriginalDomain.size()
                    ? resultEntryOriginalDomain[i]
                    : entry.domain;
            if (!entry.target.valid() ||
                originalDomain != ControlDomain::Combinational ||
                hasSlices ||
                entry.target.index >= widthBySymbol.size() ||
                widthBySymbol[entry.target.index] <= 0)
            {
                continue;
            }
            entryBySymbol[entry.target.index] = i;
        }
        if (entryBySymbol.empty())
        {
            return false;
        }

        std::unordered_map<PlanIndex, std::vector<PlanIndex>> edges;
        edges.reserve(entryBySymbol.size());
        for (const auto& item : entryBySymbol)
        {
            const PlanIndex symbol = item.first;
            const WriteBackPlan::Entry& entry = result.entries[item.second];
            auto directSymbols = collectDirectSymbols(entry.nextValue);
            for (PlanIndex used : directSymbols)
            {
                if (entryBySymbol.find(used) != entryBySymbol.end())
                {
                    edges[symbol].push_back(used);
                }
            }
            if (edges.find(symbol) == edges.end())
            {
                edges.emplace(symbol, std::vector<PlanIndex>{});
            }
        }

        std::unordered_map<PlanIndex, int32_t> index;
        std::unordered_map<PlanIndex, int32_t> lowlink;
        std::unordered_set<PlanIndex> onStack;
        std::vector<PlanIndex> stack;
        int32_t nextIndex = 0;
        bool changed = false;

        std::function<void(PlanIndex)> visit = [&](PlanIndex symbol) {
            index[symbol] = nextIndex;
            lowlink[symbol] = nextIndex;
            ++nextIndex;
            stack.push_back(symbol);
            onStack.insert(symbol);

            for (PlanIndex succ : edges[symbol])
            {
                if (index.find(succ) == index.end())
                {
                    visit(succ);
                    lowlink[symbol] = std::min(lowlink[symbol], lowlink[succ]);
                }
                else if (onStack.find(succ) != onStack.end())
                {
                    lowlink[symbol] = std::min(lowlink[symbol], index[succ]);
                }
            }

            if (lowlink[symbol] != index[symbol])
            {
                return;
            }

            std::vector<PlanIndex> scc;
            while (!stack.empty())
            {
                PlanIndex current = stack.back();
                stack.pop_back();
                onStack.erase(current);
                scc.push_back(current);
                if (current == symbol)
                {
                    break;
                }
            }
            bool selfLoop = false;
            if (scc.size() == 1)
            {
                const auto it = edges.find(scc.front());
                if (it != edges.end())
                {
                    selfLoop = std::find(it->second.begin(), it->second.end(),
                                         scc.front()) != it->second.end();
                }
            }
            if (scc.size() <= 1 && !selfLoop)
            {
                return;
            }

            std::unordered_set<PlanIndex> targets(scc.begin(), scc.end());
            std::vector<std::pair<PlanIndex, ExprNodeId>> replacements;
            replacements.reserve(scc.size());
            for (PlanIndex member : scc)
            {
                auto entryIt = entryBySymbol.find(member);
                if (entryIt == entryBySymbol.end())
                {
                    replacements.clear();
                    break;
                }
                WriteBackPlan::Entry& entry = result.entries[entryIt->second];
                ExprNodeId projected =
                    replaceStaticSlicesForSymbolSet(entry.nextValue, targets);
                if (projected == kInvalidPlanIndex ||
                    exprUsesAnySymbolThroughCombDefinitions(projected, targets))
                {
                    replacements.clear();
                    break;
                }
                replacements.emplace_back(member, projected);
            }
            if (replacements.empty())
            {
                return;
            }
            for (const auto& replacement : replacements)
            {
                auto entryIt = entryBySymbol.find(replacement.first);
                if (entryIt == entryBySymbol.end())
                {
                    continue;
                }
                WriteBackPlan::Entry& entry = result.entries[entryIt->second];
                const ControlDomain originalDomain =
                    entryIt->second < resultEntryOriginalDomain.size()
                        ? resultEntryOriginalDomain[entryIt->second]
                        : entry.domain;
                entry.nextValue = replacement.second;
                entry.domain =
                    finalizeDomain(originalDomain, false, entry.updateCond, false);
                if (entry.domain == ControlDomain::Combinational &&
                    entry.target.index < combDefinitionBySymbol.size())
                {
                    combDefinitionBySymbol[entry.target.index] = entry.nextValue;
                }
                changed = true;
            }
        };

        for (const auto& item : entryBySymbol)
        {
            if (index.find(item.first) == index.end())
            {
                visit(item.first);
            }
        }
        return changed;
    };

    for (std::size_t pass = 0; pass < 8; ++pass)
    {
        rebuildCombDefinitionMap(true);
        bool changed = false;
        if (projectCombSymbolSccs())
        {
            changed = true;
        }
        for (std::size_t i = 0; i < result.entries.size(); ++i)
        {
            if (i < resultEntryHasSlices.size() && resultEntryHasSlices[i] != 0)
            {
                continue;
            }
            WriteBackPlan::Entry& entry = result.entries[i];
            const ControlDomain originalDomain =
                i < resultEntryOriginalDomain.size()
                    ? resultEntryOriginalDomain[i]
                    : entry.domain;
            if (originalDomain != ControlDomain::Combinational ||
                !entry.target.valid() ||
                entry.target.index >= widthBySymbol.size() ||
                widthBySymbol[entry.target.index] <= 0 ||
                !exprUsesSymbolThroughCombDefinitions(entry.nextValue, entry.target))
            {
                continue;
            }
            ExprNodeId projected = replaceTargetStaticSlicesWithProjection(
                entry.nextValue, entry.target, widthBySymbol[entry.target.index]);
            if (projected == kInvalidPlanIndex ||
                exprUsesSymbolThroughCombDefinitions(projected, entry.target))
            {
                continue;
            }
            entry.nextValue = projected;
            entry.domain =
                finalizeDomain(originalDomain, false, entry.updateCond, false);
            if (entry.domain == ControlDomain::Combinational)
            {
                if (entry.target.index >= combDefinitionBySymbol.size())
                {
                    combDefinitionBySymbol.resize(entry.target.index + 1,
                                                  kInvalidPlanIndex);
                }
                combDefinitionBySymbol[entry.target.index] = entry.nextValue;
            }
            changed = true;
        }
        if (!changed)
        {
            break;
        }
    }

    if (!lowering.memoryReads.empty())
    {
        std::unordered_map<uint64_t, bool> exprEqMemo;
        exprEqMemo.reserve(lowering.values.size() * 2 + 8);

        auto exprEquivalent = [&](auto&& self, ExprNodeId lhs, ExprNodeId rhs) -> bool {
            if (lhs == rhs)
            {
                return true;
            }
            if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
            {
                return false;
            }
            if (lhs >= lowering.values.size() || rhs >= lowering.values.size())
            {
                return false;
            }
            const uint64_t key = (static_cast<uint64_t>(lhs) << 32) | rhs;
            if (auto it = exprEqMemo.find(key); it != exprEqMemo.end())
            {
                return it->second;
            }
            const ExprNode& lhsNode = lowering.values[lhs];
            const ExprNode& rhsNode = lowering.values[rhs];
            if (lhsNode.kind != rhsNode.kind)
            {
                exprEqMemo.emplace(key, false);
                return false;
            }
            bool result = false;
            switch (lhsNode.kind)
            {
            case ExprNodeKind::Invalid:
                result = true;
                break;
            case ExprNodeKind::Constant:
                result = lhsNode.literal == rhsNode.literal;
                break;
            case ExprNodeKind::Symbol:
                result = lhsNode.symbol.index == rhsNode.symbol.index;
                break;
            case ExprNodeKind::XmrRead:
                result = lhsNode.xmrPath == rhsNode.xmrPath;
                break;
            case ExprNodeKind::Operation:
                if (lhsNode.op != rhsNode.op ||
                    lhsNode.operands.size() != rhsNode.operands.size())
                {
                    result = false;
                    break;
                }
                result = true;
                for (std::size_t i = 0; i < lhsNode.operands.size(); ++i)
                {
                    if (!self(self, lhsNode.operands[i], rhsNode.operands[i]))
                    {
                        result = false;
                        break;
                    }
                }
                break;
            }
            exprEqMemo.emplace(key, result);
            return result;
        };

        auto exprListEquivalent = [&](const std::vector<ExprNodeId>& lhs,
                                      const std::vector<ExprNodeId>& rhs) -> bool {
            if (lhs.size() != rhs.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (!exprEquivalent(exprEquivalent, lhs[i], rhs[i]))
                {
                    return false;
                }
            }
            return true;
        };

        std::vector<const MemoryReadPort*> syncReads;
        syncReads.reserve(lowering.memoryReads.size());
        for (const auto& read : lowering.memoryReads)
        {
            if (!read.isSync || read.data == kInvalidPlanIndex)
            {
                continue;
            }
            syncReads.push_back(&read);
        }

        if (!syncReads.empty())
        {
            for (auto& entry : result.entries)
            {
                if (entry.domain != ControlDomain::Sequential)
                {
                    continue;
                }
                for (const MemoryReadPort* read : syncReads)
                {
                    if (!exprEquivalent(exprEquivalent, entry.nextValue, read->data))
                    {
                        continue;
                    }
                    if (!exprEquivalent(exprEquivalent, entry.updateCond, read->updateCond))
                    {
                        continue;
                    }
                    if (entry.eventEdges != read->eventEdges ||
                        !exprListEquivalent(entry.eventOperands, read->eventOperands))
                    {
                        continue;
                    }
                    entry.domain = ControlDomain::Combinational;
                    entry.updateCond = kInvalidPlanIndex;
                    entry.eventEdges.clear();
                    entry.eventOperands.clear();
                    break;
                }
            }
        }
    }

    return result;
}

namespace {

class MemoryPortBuilder {
public:
    MemoryPortBuilder(ModulePlan& plan, LoweringPlan& lowering)
        : plan_(plan), lowering_(lowering)
    {
    }

    ExprNodeId ensureGuardExpr(ExprNodeId guard, slang::SourceLocation location)
    {
        if (guard != kInvalidPlanIndex)
        {
            return guard;
        }
        if (constOne_.has_value())
        {
            return *constOne_;
        }
        ExprNodeId id = addConstantLiteral("1'b1", location);
        constOne_ = id;
        return id;
    }

    ExprNodeId addConstantLiteral(std::string literal, slang::SourceLocation location)
    {
        ExprNode node;
        node.kind = ExprNodeKind::Constant;
        node.literal = std::move(literal);
        node.location = location;
        return addNode(std::move(node));
    }

    ExprNodeId makeOperation(wolvrix::lib::grh::OperationKind op, std::vector<ExprNodeId> operands,
                             slang::SourceLocation location)
    {
        return makeOperationWithWidth(op, std::move(operands), 0, location);
    }

    ExprNodeId makeOperationWithWidth(wolvrix::lib::grh::OperationKind op,
                                      std::vector<ExprNodeId> operands,
                                      int32_t widthHint,
                                      slang::SourceLocation location)
    {
        for (ExprNodeId operand : operands)
        {
            if (operand == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
        }
        ExprNode node;
        node.kind = ExprNodeKind::Operation;
        node.op = op;
        node.operands = std::move(operands);
        node.location = location;
        node.tempSymbol = makeTempSymbol();
        if (widthHint > 0)
        {
            node.widthHint = widthHint;
        }
        return addNode(std::move(node));
    }

    ExprNodeId makeSliceDynamic(ExprNodeId value, ExprNodeId index, int32_t width,
                                slang::SourceLocation location)
    {
        return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                                      {value, index}, width, location);
    }

    ExprNodeId resizeValueToWidth(ExprNodeId value, int32_t targetWidth,
                                  bool signExtend, slang::SourceLocation location)
    {
        if (value == kInvalidPlanIndex || targetWidth <= 0)
        {
            return value;
        }
        int32_t sourceWidth = 0;
        if (value < lowering_.values.size())
        {
            sourceWidth = lowering_.values[value].widthHint;
        }
        if (sourceWidth == targetWidth)
        {
            return value;
        }
        if (sourceWidth <= 0)
        {
            return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kAssign,
                                          {value}, targetWidth, location);
        }
        if (targetWidth < sourceWidth)
        {
            ExprNodeId zeroIndex = addConstantLiteral("0", location);
            return makeSliceDynamic(value, zeroIndex, targetWidth, location);
        }
        const int32_t padWidth = targetWidth - sourceWidth;
        ExprNodeId pad = kInvalidPlanIndex;
        if (signExtend && sourceWidth > 0)
        {
            ExprNodeId signIndex = addConstantLiteral(std::to_string(sourceWidth - 1),
                                                      location);
            ExprNodeId signBit = makeSliceDynamic(value, signIndex, 1, location);
            ExprNodeId count = addConstantLiteral(std::to_string(padWidth), location);
            pad = makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kReplicate,
                                         {count, signBit}, padWidth, location);
        }
        else
        {
            pad = addConstantLiteral(std::to_string(padWidth) + "'b0", location);
            if (pad != kInvalidPlanIndex && pad < lowering_.values.size())
            {
                lowering_.values[pad].widthHint = padWidth;
            }
        }
        if (pad == kInvalidPlanIndex)
        {
            return value;
        }
        return makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kConcat,
                                      {pad, value}, targetWidth, location);
    }

private:
    PlanSymbolId makeTempSymbol()
    {
        PlanSymbolId id = makeInternalPlanValueSymbol(plan_);
        lowering_.tempSymbols.push_back(id);
        return id;
    }

    ExprNodeId addNode(ExprNode node)
    {
        const ExprNodeId id = static_cast<ExprNodeId>(lowering_.values.size());
        lowering_.values.push_back(std::move(node));
        return id;
    }

    ModulePlan& plan_;
    LoweringPlan& lowering_;
    std::optional<ExprNodeId> constOne_;
};

std::optional<int64_t> evalConstInt(const ModulePlan& plan, const LoweringPlan& lowering,
                                    ExprNodeId id)
{
    std::unordered_set<ExprNodeId> visited;
    std::unordered_set<ExprNodeId> visitedSv;
    auto evalParamValue = [](const slang::ast::ParameterSymbol& param)
        -> std::optional<slang::SVInt> {
        slang::ConstantValue value = param.getValue();
        if (value.bad())
        {
            return std::nullopt;
        }
        if (!value.isInteger())
        {
            value = value.convertToInt();
        }
        if (!value.isInteger())
        {
            return std::nullopt;
        }
        const slang::SVInt& literal = value.integer();
        if (literal.hasUnknown())
        {
            return std::nullopt;
        }
        return literal;
    };
    auto lookupParamValue = [&](std::string_view name) -> std::optional<slang::SVInt> {
        if (!plan.body)
        {
            return std::nullopt;
        }
        for (const slang::ast::ParameterSymbolBase* paramBase :
             plan.body->getParameters())
        {
            if (!paramBase || paramBase->symbol.name != name)
            {
                continue;
            }
            const auto* valueParam =
                paramBase->symbol.as_if<slang::ast::ParameterSymbol>();
            if (!valueParam)
            {
                return std::nullopt;
            }
            return evalParamValue(*valueParam);
        }
        if (const slang::ast::Symbol* symbol = plan.body->find(name))
        {
            if (const auto* param = symbol->as_if<slang::ast::ParameterSymbol>())
            {
                return evalParamValue(*param);
            }
        }
        const slang::ast::RootSymbol& root = plan.body->getCompilation().getRoot();
        const slang::ast::ParameterSymbol* matched = nullptr;
        auto checkPackage = [&](const slang::ast::PackageSymbol& package) -> bool {
            const slang::ast::Symbol* symbol = package.findForImport(name);
            if (!symbol)
            {
                return true;
            }
            const auto* param = symbol->as_if<slang::ast::ParameterSymbol>();
            if (!param)
            {
                return false;
            }
            if (matched && matched != param)
            {
                return false;
            }
            matched = param;
            return true;
        };
        for (const auto& package : root.membersOfType<slang::ast::PackageSymbol>())
        {
            if (!checkPackage(package))
            {
                return std::nullopt;
            }
        }
        for (const auto& unit : root.membersOfType<slang::ast::CompilationUnitSymbol>())
        {
            for (const auto& package : unit.membersOfType<slang::ast::PackageSymbol>())
            {
                if (!checkPackage(package))
                {
                    return std::nullopt;
                }
            }
        }
        if (matched)
        {
            return evalParamValue(*matched);
        }
        return std::nullopt;
    };
    auto evalConstSVInt = [&](auto&& self, ExprNodeId nodeId)
        -> std::optional<slang::SVInt> {
        if (nodeId == kInvalidPlanIndex || nodeId >= lowering.values.size())
        {
            return std::nullopt;
        }
        if (!visitedSv.insert(nodeId).second)
        {
            return std::nullopt;
        }
        const ExprNode& node = lowering.values[nodeId];
        if (node.kind == ExprNodeKind::Constant)
        {
            slang::SVInt literal = slang::SVInt::fromString(node.literal);
            if (literal.hasUnknown())
            {
                return std::nullopt;
            }
            if (node.widthHint > 0 &&
                static_cast<uint64_t>(node.widthHint) != literal.getBitWidth())
            {
                literal = literal.resize(static_cast<slang::bitwidth_t>(node.widthHint));
            }
            return literal;
        }
        if (node.kind == ExprNodeKind::Symbol)
        {
            if (!node.symbol.valid())
            {
                return std::nullopt;
            }
            auto value = lookupParamValue(plan.symbolTable.text(node.symbol));
            if (value && node.widthHint > 0 &&
                static_cast<uint64_t>(node.widthHint) != value->getBitWidth())
            {
                *value = value->resize(static_cast<slang::bitwidth_t>(node.widthHint));
            }
            return value;
        }
        if (node.kind != ExprNodeKind::Operation)
        {
            return std::nullopt;
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kMux && node.operands.size() == 3)
        {
            auto cond = self(self, node.operands[0]);
            if (!cond)
            {
                return std::nullopt;
            }
            slang::logic_t condBit = cond->reductionOr();
            if (condBit.isUnknown())
            {
                return std::nullopt;
            }
            ExprNodeId branch = static_cast<bool>(condBit) ? node.operands[1] : node.operands[2];
            return self(self, branch);
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kConcat)
        {
            std::vector<slang::SVInt> operands;
            operands.reserve(node.operands.size());
            for (ExprNodeId operand : node.operands)
            {
                auto value = self(self, operand);
                if (!value)
                {
                    return std::nullopt;
                }
                operands.push_back(*value);
            }
            if (operands.empty())
            {
                return std::nullopt;
            }
            slang::SVInt result = slang::SVInt::concat(operands);
            if (node.widthHint > 0 &&
                static_cast<uint64_t>(node.widthHint) != result.getBitWidth())
            {
                result = result.resize(static_cast<slang::bitwidth_t>(node.widthHint));
            }
            return result;
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kReplicate &&
            node.operands.size() >= 2)
        {
            auto countValue = self(self, node.operands[0]);
            auto dataValue = self(self, node.operands[1]);
            if (!countValue || !dataValue)
            {
                return std::nullopt;
            }
            auto countInt = countValue->template as<int64_t>();
            if (!countInt || *countInt < 0)
            {
                return std::nullopt;
            }
            slang::SVInt result = dataValue->replicate(*countValue);
            if (node.widthHint > 0 &&
                static_cast<uint64_t>(node.widthHint) != result.getBitWidth())
            {
                result = result.resize(static_cast<slang::bitwidth_t>(node.widthHint));
            }
            return result;
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
            node.operands.size() >= 2)
        {
            auto dataValue = self(self, node.operands[0]);
            auto offsetValue = self(self, node.operands[1]);
            if (!dataValue || !offsetValue)
            {
                return std::nullopt;
            }
            auto offset = offsetValue->template as<int64_t>();
            if (!offset || *offset < 0)
            {
                return std::nullopt;
            }
            if (node.widthHint <= 0)
            {
                return std::nullopt;
            }
            const uint64_t width = static_cast<uint64_t>(node.widthHint);
            slang::SVInt result =
                dataValue->lshr(static_cast<slang::bitwidth_t>(*offset))
                    .trunc(static_cast<slang::bitwidth_t>(width));
            return result;
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kSliceArray &&
            node.operands.size() >= 2)
        {
            auto dataValue = self(self, node.operands[0]);
            auto indexValue = self(self, node.operands[1]);
            if (!dataValue || !indexValue)
            {
                return std::nullopt;
            }
            auto index = indexValue->template as<int64_t>();
            if (!index || *index < 0)
            {
                return std::nullopt;
            }
            const int32_t elementWidth = node.sliceWidth > 0 ? node.sliceWidth : node.widthHint;
            if (elementWidth <= 0)
            {
                return std::nullopt;
            }
            const int64_t offset = *index * static_cast<int64_t>(elementWidth);
            if (offset < 0 ||
                offset > static_cast<int64_t>(std::numeric_limits<slang::bitwidth_t>::max()))
            {
                return std::nullopt;
            }
            slang::SVInt result =
                dataValue->lshr(static_cast<slang::bitwidth_t>(offset))
                    .trunc(static_cast<slang::bitwidth_t>(elementWidth));
            return result;
        }
        if (node.operands.size() == 1)
        {
            auto operand = self(self, node.operands.front());
            if (!operand)
            {
                return std::nullopt;
            }
            switch (node.op)
            {
            case wolvrix::lib::grh::OperationKind::kNot:
                return ~(*operand);
            case wolvrix::lib::grh::OperationKind::kLogicNot:
            {
                slang::logic_t bit = operand->reductionOr();
                if (bit.isUnknown())
                {
                    return std::nullopt;
                }
                return slang::SVInt(1, static_cast<uint64_t>(!static_cast<bool>(bit)),
                                    false);
            }
            default:
                break;
            }
            return std::nullopt;
        }
        if (node.operands.size() != 2)
        {
            return std::nullopt;
        }
        auto lhs = self(self, node.operands[0]);
        auto rhs = self(self, node.operands[1]);
        if (!lhs || !rhs)
        {
            return std::nullopt;
        }
        auto widenBinaryOperands = [&]() {
            if (node.widthHint <= 0)
            {
                return;
            }
            const slang::bitwidth_t width =
                static_cast<slang::bitwidth_t>(node.widthHint);
            if (lhs->getBitWidth() < width)
            {
                *lhs = lhs->resize(width);
            }
            if (rhs->getBitWidth() < width)
            {
                *rhs = rhs->resize(width);
            }
        };
        auto applyWidthHint = [&](slang::SVInt value) -> slang::SVInt {
            if (node.widthHint > 0 &&
                static_cast<uint64_t>(node.widthHint) != value.getBitWidth())
            {
                return value.resize(static_cast<slang::bitwidth_t>(node.widthHint));
            }
            return value;
        };
        switch (node.op)
        {
        case wolvrix::lib::grh::OperationKind::kAdd:
            widenBinaryOperands();
            return applyWidthHint(*lhs + *rhs);
        case wolvrix::lib::grh::OperationKind::kSub:
            widenBinaryOperands();
            return applyWidthHint(*lhs - *rhs);
        case wolvrix::lib::grh::OperationKind::kMul:
            widenBinaryOperands();
            return applyWidthHint(*lhs * *rhs);
        case wolvrix::lib::grh::OperationKind::kDiv:
            widenBinaryOperands();
            return applyWidthHint(*lhs / *rhs);
        case wolvrix::lib::grh::OperationKind::kMod:
            widenBinaryOperands();
            return applyWidthHint(*lhs % *rhs);
        case wolvrix::lib::grh::OperationKind::kAnd:
            widenBinaryOperands();
            return applyWidthHint(*lhs & *rhs);
        case wolvrix::lib::grh::OperationKind::kOr:
            widenBinaryOperands();
            return applyWidthHint(*lhs | *rhs);
        case wolvrix::lib::grh::OperationKind::kXor:
            widenBinaryOperands();
            return applyWidthHint(*lhs ^ *rhs);
        case wolvrix::lib::grh::OperationKind::kXnor:
            widenBinaryOperands();
            return applyWidthHint(lhs->xnor(*rhs));
        case wolvrix::lib::grh::OperationKind::kShl:
            return applyWidthHint(lhs->shl(*rhs));
        case wolvrix::lib::grh::OperationKind::kLShr:
            return applyWidthHint(lhs->lshr(*rhs));
        case wolvrix::lib::grh::OperationKind::kAShr:
            return applyWidthHint(lhs->ashr(*rhs));
        case wolvrix::lib::grh::OperationKind::kLogicAnd:
        {
            slang::logic_t bit = lhs->reductionOr() && rhs->reductionOr();
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        case wolvrix::lib::grh::OperationKind::kLogicOr:
        {
            slang::logic_t bit = lhs->reductionOr() || rhs->reductionOr();
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        case wolvrix::lib::grh::OperationKind::kEq:
        case wolvrix::lib::grh::OperationKind::kCaseEq:
        {
            slang::logic_t bit = *lhs == *rhs;
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        case wolvrix::lib::grh::OperationKind::kNe:
        case wolvrix::lib::grh::OperationKind::kCaseNe:
        {
            slang::logic_t bit = *lhs != *rhs;
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        case wolvrix::lib::grh::OperationKind::kLt:
        {
            slang::logic_t bit = *lhs < *rhs;
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        case wolvrix::lib::grh::OperationKind::kLe:
        {
            slang::logic_t bit = *lhs <= *rhs;
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        case wolvrix::lib::grh::OperationKind::kGt:
        {
            slang::logic_t bit = *lhs > *rhs;
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        case wolvrix::lib::grh::OperationKind::kGe:
        {
            slang::logic_t bit = *lhs >= *rhs;
            if (bit.isUnknown())
            {
                return std::nullopt;
            }
            return applyWidthHint(
                slang::SVInt(1, static_cast<uint64_t>(static_cast<bool>(bit)), false));
        }
        default:
            break;
        }
        return std::nullopt;
    };
    auto evalNode = [&](auto&& self, ExprNodeId nodeId) -> std::optional<int64_t> {
        if (nodeId == kInvalidPlanIndex || nodeId >= lowering.values.size())
        {
            return std::nullopt;
        }
        if (!visited.insert(nodeId).second)
        {
            return std::nullopt;
        }
        const ExprNode& node = lowering.values[nodeId];
        if (node.kind == ExprNodeKind::Constant)
        {
            slang::SVInt literal = slang::SVInt::fromString(node.literal);
            if (literal.hasUnknown())
            {
                return std::nullopt;
            }
            return literal.as<int64_t>();
        }
        if (node.kind == ExprNodeKind::Symbol)
        {
            if (!node.symbol.valid() || !plan.body)
            {
                return std::nullopt;
            }
            const std::string_view name = plan.symbolTable.text(node.symbol);
            auto value = lookupParamValue(name);
            if (value)
            {
                return value->as<int64_t>();
            }
            return std::nullopt;
        }
        if (node.kind != ExprNodeKind::Operation)
        {
            return std::nullopt;
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kMux && node.operands.size() == 3)
        {
            auto cond = self(self, node.operands[0]);
            if (!cond)
            {
                return std::nullopt;
            }
            ExprNodeId branch = *cond != 0 ? node.operands[1] : node.operands[2];
            return self(self, branch);
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kConcat ||
            node.op == wolvrix::lib::grh::OperationKind::kReplicate ||
            node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic ||
            node.op == wolvrix::lib::grh::OperationKind::kSliceArray)
        {
            auto value = evalConstSVInt(evalConstSVInt, nodeId);
            if (!value || value->hasUnknown())
            {
                return std::nullopt;
            }
            return value->as<int64_t>();
        }
        if (node.operands.empty())
        {
            return std::nullopt;
        }
        if (node.operands.size() == 1)
        {
            auto operand = self(self, node.operands.front());
            if (!operand)
            {
                return std::nullopt;
            }
            switch (node.op)
            {
            case wolvrix::lib::grh::OperationKind::kNot:
                return ~(*operand);
            case wolvrix::lib::grh::OperationKind::kLogicNot:
                return *operand == 0 ? 1 : 0;
            default:
                return std::nullopt;
            }
        }
        if (node.operands.size() != 2)
        {
            return std::nullopt;
        }
        auto lhs = self(self, node.operands[0]);
        if (!lhs)
        {
            return std::nullopt;
        }
        auto rhs = self(self, node.operands[1]);
        if (!rhs)
        {
            return std::nullopt;
        }
        switch (node.op)
        {
        case wolvrix::lib::grh::OperationKind::kAdd:
            return *lhs + *rhs;
        case wolvrix::lib::grh::OperationKind::kSub:
            return *lhs - *rhs;
        case wolvrix::lib::grh::OperationKind::kMul:
            return *lhs * *rhs;
        case wolvrix::lib::grh::OperationKind::kDiv:
            return *rhs == 0 ? std::nullopt : std::optional<int64_t>(*lhs / *rhs);
        case wolvrix::lib::grh::OperationKind::kMod:
            return *rhs == 0 ? std::nullopt : std::optional<int64_t>(*lhs % *rhs);
        case wolvrix::lib::grh::OperationKind::kAnd:
            return *lhs & *rhs;
        case wolvrix::lib::grh::OperationKind::kOr:
            return *lhs | *rhs;
        case wolvrix::lib::grh::OperationKind::kXor:
            return *lhs ^ *rhs;
        case wolvrix::lib::grh::OperationKind::kXnor:
            return ~(*lhs ^ *rhs);
        case wolvrix::lib::grh::OperationKind::kLogicAnd:
            return (*lhs != 0 && *rhs != 0) ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kLogicOr:
            return (*lhs != 0 || *rhs != 0) ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kEq:
        case wolvrix::lib::grh::OperationKind::kCaseEq:
            return *lhs == *rhs ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kNe:
        case wolvrix::lib::grh::OperationKind::kCaseNe:
            return *lhs != *rhs ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kLt:
            return *lhs < *rhs ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kLe:
            return *lhs <= *rhs ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kGt:
            return *lhs > *rhs ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kGe:
            return *lhs >= *rhs ? 1 : 0;
        case wolvrix::lib::grh::OperationKind::kShl:
        {
            if (*rhs < 0 || *rhs >= 63)
            {
                return std::nullopt;
            }
            return *lhs << *rhs;
        }
        case wolvrix::lib::grh::OperationKind::kLShr:
        {
            if (*rhs < 0 || *rhs >= 63)
            {
                return std::nullopt;
            }
            return static_cast<int64_t>(static_cast<uint64_t>(*lhs) >> *rhs);
        }
        case wolvrix::lib::grh::OperationKind::kAShr:
        {
            if (*rhs < 0 || *rhs >= 63)
            {
                return std::nullopt;
            }
            return *lhs >> *rhs;
        }
        default:
            break;
        }
        return std::nullopt;
    };
    return evalNode(evalNode, id);
}

std::optional<std::string> makeMaskLiteral(int64_t width, int64_t lo, int64_t hi)
{
    if (width <= 0 || lo < 0 || hi < lo || hi >= width)
    {
        return std::nullopt;
    }
    std::string bits(static_cast<std::size_t>(width), '0');
    for (int64_t i = lo; i <= hi; ++i)
    {
        const std::size_t index = static_cast<std::size_t>(width - 1 - i);
        bits[index] = '1';
    }
    std::string literal;
    literal.reserve(static_cast<std::size_t>(width) + 8);
    literal.append(std::to_string(width));
    literal.append("'b");
    literal.append(bits);
    return literal;
}

struct MemoryReadUse {
    PlanSymbolId memory;
    SignalId signal = kInvalidPlanIndex;
    ExprNodeId data = kInvalidPlanIndex;
    ExprNodeId replacement = kInvalidPlanIndex;
    ControlDomain domain = ControlDomain::Unknown;
    ExprNodeId updateCond = kInvalidPlanIndex;
    std::vector<ExprNodeId> addressIndices;
    std::vector<EventEdge> eventEdges;
    std::vector<ExprNodeId> eventOperands;
    slang::SourceLocation location{};
};

struct IndexedMemoryWriteMergeCandidate {
    std::size_t writeIndex = 0;
    int64_t addressConst = 0;
    ExprNodeId dynamicAddress = kInvalidPlanIndex;
    std::vector<ExprNodeId> enableTerms;
};

} // namespace

void MemoryPortLowererPass::lower(ModulePlan& plan, LoweringPlan& lowering)
{
    if (!plan.body)
    {
        return;
    }

    lowering.memoryReads.clear();
    lowering.memoryWrites.clear();

    std::vector<SignalId> signalBySymbol(plan.symbolTable.size(), kInvalidPlanIndex);
    for (SignalId i = 0; i < static_cast<SignalId>(plan.signals.size()); ++i)
    {
        const PlanSymbolId id = plan.signals[i].symbol;
        if (id.valid() && id.index < signalBySymbol.size())
        {
            signalBySymbol[id.index] = i;
        }
    }

    auto resolveMemorySignal = [&](PlanSymbolId symbol) -> SignalId {
        if (!symbol.valid() || symbol.index >= signalBySymbol.size())
        {
            return kInvalidPlanIndex;
        }
        SignalId id = signalBySymbol[symbol.index];
        if (id == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        if (plan.signals[id].memoryRows <= 0 ||
            plan.signals[id].kind == SignalKind::Net)
        {
            return kInvalidPlanIndex;
        }
        return id;
    };

    auto isPackedAggregateSymbol = [&](PlanSymbolId symbol) -> bool {
        if (!symbol.valid() || symbol.index >= signalBySymbol.size())
        {
            return false;
        }
        const SignalId signal = signalBySymbol[symbol.index];
        return signal != kInvalidPlanIndex &&
               signal < static_cast<SignalId>(plan.signals.size()) &&
               isPackedAggregateVariable(plan.signals[signal]);
    };

    auto getMemoryReadCandidate = [&](ExprNodeId id, MemoryReadUse& out) -> bool {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return false;
        }
        ExprNodeId current = id;
        std::vector<ExprNodeId> indices;
        ExprNodeId replacement = id;
        while (current != kInvalidPlanIndex && current < lowering.values.size())
        {
            const ExprNode& node = lowering.values[current];
            if (node.kind == ExprNodeKind::Operation &&
                node.op == wolvrix::lib::grh::OperationKind::kAssign &&
                node.operands.size() == 1)
            {
                current = node.operands.front();
                continue;
            }
            if (node.kind != ExprNodeKind::Operation ||
                node.operands.size() < 2)
            {
                break;
            }
            const bool isSliceAddress =
                node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic ||
                node.op == wolvrix::lib::grh::OperationKind::kSliceArray;
            if (!isSliceAddress)
            {
                break;
            }
            if (node.operands.size() == 2)
            {
                indices.push_back(node.operands[1]);
            }
            const ExprNodeId next = node.operands[0];
            if (next != kInvalidPlanIndex && next < lowering.values.size() &&
                lowering.values[next].kind == ExprNodeKind::Symbol)
            {
                if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic &&
                    isPackedAggregateSymbol(lowering.values[next].symbol))
                {
                    return false;
                }
                replacement = current;
                current = next;
                break;
            }
            current = next;
        }
        if (current == kInvalidPlanIndex || current >= lowering.values.size())
        {
            return false;
        }
        const ExprNode& baseNode = lowering.values[current];
        if (baseNode.kind != ExprNodeKind::Symbol)
        {
            return false;
        }
        SignalId signal = resolveMemorySignal(baseNode.symbol);
        if (signal == kInvalidPlanIndex)
        {
            return false;
        }
        if (!indices.empty())
        {
            std::reverse(indices.begin(), indices.end());
        }
        const auto& dims = plan.signals[signal].unpackedDims;
        const std::size_t addressCount = dims.empty() ? 1 : dims.size();
        if (indices.size() < addressCount)
        {
            return false;
        }
        if (indices.size() > addressCount)
        {
            indices.resize(addressCount);
        }
        out.memory = baseNode.symbol;
        out.signal = signal;
        out.data = id;
        out.replacement = replacement;
        out.addressIndices = std::move(indices);
        out.location = baseNode.location.valid() ? baseNode.location : lowering.values[id].location;
        return true;
    };

    std::vector<MemoryReadUse> readUses;
    auto recordReadUse = [&](const MemoryReadUse& candidate) {
        for (const auto& existing : readUses)
        {
            if (existing.memory.index == candidate.memory.index &&
                existing.domain == candidate.domain &&
                existing.updateCond == candidate.updateCond &&
                existing.addressIndices == candidate.addressIndices &&
                existing.eventEdges == candidate.eventEdges &&
                existing.eventOperands == candidate.eventOperands &&
                existing.replacement == candidate.replacement)
            {
                return;
            }
        }
        readUses.push_back(candidate);
    };

    MemoryPortBuilder builder(plan, lowering);

    auto exprDependsOn = [&](ExprNodeId root, ExprNodeId needle) -> bool {
        if (root == kInvalidPlanIndex || needle == kInvalidPlanIndex)
        {
            return false;
        }
        if (root == needle)
        {
            return true;
        }
        std::unordered_set<ExprNodeId> seen;
        std::vector<ExprNodeId> stack;
        stack.push_back(root);
        while (!stack.empty())
        {
            const ExprNodeId current = stack.back();
            stack.pop_back();
            if (current == needle)
            {
                return true;
            }
            if (!seen.insert(current).second)
            {
                continue;
            }
            if (current == kInvalidPlanIndex ||
                current >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                continue;
            }
            const ExprNode& node = lowering.values[current];
            if (node.kind != ExprNodeKind::Operation)
            {
                continue;
            }
            for (ExprNodeId operand : node.operands)
            {
                if (operand != kInvalidPlanIndex)
                {
                    stack.push_back(operand);
                }
            }
        }
        return false;
    };

    auto visitExpr = [&](auto&& self, ExprNodeId id, ControlDomain domain,
                         const std::vector<EventEdge>& edges,
                         const std::vector<ExprNodeId>& operands,
                         ExprNodeId updateCond,
                         slang::SourceLocation location,
                         std::unordered_set<ExprNodeId>& visited) -> void {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return;
        }
        if (!visited.insert(id).second)
        {
            return;
        }
        MemoryReadUse candidate;
        const bool isMemorySlice = getMemoryReadCandidate(id, candidate);
        if (isMemorySlice)
        {
            candidate.domain = domain;
            candidate.updateCond = updateCond;
            candidate.eventEdges = edges;
            candidate.eventOperands = operands;
            candidate.location = location;
            if (candidate.domain == ControlDomain::Sequential &&
                exprDependsOn(candidate.updateCond, candidate.data))
            {
                candidate.updateCond =
                    builder.ensureGuardExpr(kInvalidPlanIndex, candidate.location);
            }
            recordReadUse(candidate);
        }
        const ExprNode& node = lowering.values[id];
        if (node.kind == ExprNodeKind::Operation)
        {
            const bool isSliceAddress =
                node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic ||
                node.op == wolvrix::lib::grh::OperationKind::kSliceArray;
            if (isSliceAddress && isMemorySlice && !node.operands.empty())
            {
                for (std::size_t i = 1; i < node.operands.size(); ++i)
                {
                    ExprNodeId operand = node.operands[i];
                    self(self, operand, domain, edges, operands, updateCond, location, visited);
                }
            }
            else
            {
                for (ExprNodeId operand : node.operands)
                {
                    self(self, operand, domain, edges, operands, updateCond, location, visited);
                }
            }
        }
    };

    for (const auto& stmt : lowering.loweredStmts)
    {
        if (stmt.kind != LoweredStmtKind::Write)
        {
            continue;
        }
        if (stmt.write.isXmr)
        {
            continue;
        }
        std::unordered_set<ExprNodeId> visited;
        const ControlDomain domain = stmt.write.domain;
        ExprNodeId updateCond = kInvalidPlanIndex;
        if (domain == ControlDomain::Sequential)
        {
            updateCond = builder.ensureGuardExpr(stmt.write.guard, stmt.location);
        }
        if (stmt.write.value != kInvalidPlanIndex)
        {
            visitExpr(visitExpr, stmt.write.value, domain, stmt.eventEdges,
                      stmt.eventOperands, updateCond, stmt.location, visited);
        }
        if (stmt.write.guard != kInvalidPlanIndex)
        {
            visitExpr(visitExpr, stmt.write.guard, domain, stmt.eventEdges,
                      stmt.eventOperands, updateCond, stmt.location, visited);
        }
    }

    if (!readUses.empty())
    {
        for (const auto& stmt : lowering.loweredStmts)
        {
            if (stmt.kind != LoweredStmtKind::Write)
            {
                continue;
            }
            const WriteIntent& write = stmt.write;
            if (write.isXmr)
            {
                continue;
            }
            if (write.domain != ControlDomain::Sequential)
            {
                continue;
            }
            SignalId signal = resolveMemorySignal(write.target);
            if (signal == kInvalidPlanIndex)
            {
                continue;
            }
            for (auto& use : readUses)
            {
                if (use.domain != ControlDomain::Sequential)
                {
                    continue;
                }
                if (use.memory.index != write.target.index)
                {
                    continue;
                }
                bool usedInWrite = false;
                if (write.value != kInvalidPlanIndex &&
                    exprDependsOn(write.value, use.data))
                {
                    usedInWrite = true;
                }
                if (!usedInWrite && write.guard != kInvalidPlanIndex &&
                    exprDependsOn(write.guard, use.data))
                {
                    usedInWrite = true;
                }
                if (!usedInWrite)
                {
                    continue;
                }
                use.domain = ControlDomain::Combinational;
                use.updateCond = kInvalidPlanIndex;
                use.eventEdges.clear();
                use.eventOperands.clear();
            }
        }

        std::vector<MemoryReadUse> deduped;
        deduped.reserve(readUses.size());
        for (const auto& use : readUses)
        {
            bool exists = false;
            for (const auto& existing : deduped)
            {
                if (existing.memory.index == use.memory.index &&
                    existing.domain == use.domain &&
                    existing.updateCond == use.updateCond &&
                    existing.addressIndices == use.addressIndices &&
                    existing.eventEdges == use.eventEdges &&
                    existing.eventOperands == use.eventOperands &&
                    existing.replacement == use.replacement)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
            {
                deduped.push_back(use);
            }
        }
        readUses.swap(deduped);
    }

    auto buildLinearAddress = [&](std::span<const ExprNodeId> indices,
                                  std::span<const UnpackedDimInfo> dims,
                                  slang::SourceLocation location) -> ExprNodeId {
        if (indices.size() < dims.size() || dims.empty())
        {
            return kInvalidPlanIndex;
        }
        ExprNodeId address = kInvalidPlanIndex;
        int64_t stride = 1;
        std::vector<int64_t> strides(dims.size(), 1);
        for (std::size_t i = dims.size(); i-- > 0;)
        {
            strides[i] = stride;
            const int32_t extent = dims[i].extent;
            if (extent > 0 && stride <= std::numeric_limits<int64_t>::max() / extent)
            {
                stride *= extent;
            }
        }
        for (std::size_t i = 0; i < dims.size(); ++i)
        {
            ExprNodeId term = indices[i];
            const UnpackedDimInfo& dim = dims[i];
            if (dim.left < dim.right)
            {
                if (dim.left != 0)
                {
                    ExprNodeId offset =
                        builder.addConstantLiteral(std::to_string(dim.left), location);
                    term = builder.makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                                 {term, offset}, location);
                }
            }
            else
            {
                ExprNodeId offset =
                    builder.addConstantLiteral(std::to_string(dim.left), location);
                term = builder.makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                             {offset, term}, location);
            }
            if (strides[i] != 1)
            {
                ExprNodeId strideId =
                    builder.addConstantLiteral(std::to_string(strides[i]), location);
                term = builder.makeOperation(wolvrix::lib::grh::OperationKind::kMul,
                                             {term, strideId}, location);
            }
            if (address == kInvalidPlanIndex)
            {
                address = term;
            }
            else
            {
                address = builder.makeOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                {address, term}, location);
            }
        }
        return address;
    };

    std::vector<uint8_t> indexedMemoryAccess(plan.symbolTable.size(), 0);
    for (const auto& use : readUses)
    {
        if (use.memory.valid() && use.memory.index < indexedMemoryAccess.size())
        {
            indexedMemoryAccess[use.memory.index] = 1;
        }
    }
    for (const auto& stmt : lowering.loweredStmts)
    {
        if (stmt.kind != LoweredStmtKind::Write || stmt.write.isXmr)
        {
            continue;
        }
        SignalId signal = resolveMemorySignal(stmt.write.target);
        if (signal == kInvalidPlanIndex || !stmt.write.target.valid() ||
            stmt.write.target.index >= indexedMemoryAccess.size())
        {
            continue;
        }
        const auto& dims = plan.signals[signal].unpackedDims;
        const std::size_t addressCount = dims.empty() ? 1 : dims.size();
        if (stmt.write.slices.size() >= addressCount)
        {
            indexedMemoryAccess[stmt.write.target.index] = 1;
        }
    }

    for (const auto& use : readUses)
    {
        if (use.data == kInvalidPlanIndex)
        {
            continue;
        }
        if (use.domain == ControlDomain::Sequential && use.eventEdges.empty())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(use.location,
                                           "Skipping synchronous memory read without edge-sensitive timing control");
            }
            continue;
        }
        const auto& dims = plan.signals[use.signal].unpackedDims;
        ExprNodeId address = kInvalidPlanIndex;
        if (dims.empty())
        {
            if (!use.addressIndices.empty())
            {
                address = use.addressIndices.front();
            }
        }
        else
        {
            address = buildLinearAddress(use.addressIndices, dims, use.location);
        }
        if (address == kInvalidPlanIndex)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(use.location,
                                            "Memory read missing address indices");
            }
            continue;
        }

        MemoryReadPort entry;
        entry.memory = use.memory;
        entry.signal = use.signal;
        entry.address = address;
        entry.data = use.data;
        entry.replacement = use.replacement;
        entry.isSync = use.domain == ControlDomain::Sequential;
        entry.updateCond = use.updateCond;
        entry.eventEdges = use.eventEdges;
        entry.eventOperands = use.eventOperands;
        entry.location = use.location;
        lowering.memoryReads.push_back(std::move(entry));
    }

    for (const auto& stmt : lowering.loweredStmts)
    {
        if (stmt.kind != LoweredStmtKind::Write)
        {
            continue;
        }
        const WriteIntent& write = stmt.write;
        if (write.isXmr)
        {
            continue;
        }
        SignalId signal = resolveMemorySignal(write.target);
        if (signal == kInvalidPlanIndex)
        {
            continue;
        }
        const int64_t memWidth = plan.signals[signal].width > 0 ? plan.signals[signal].width : 0;
        const int64_t aggregateWidth =
            isPackedAggregateVariable(plan.signals[signal])
                ? flattenedAggregateWidth(plan.signals[signal])
                : memWidth;
        if (memWidth <= 0)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(write.location, "Memory write missing valid width");
            }
            continue;
        }
        const int32_t memWidthHint =
            static_cast<int32_t>(std::min<int64_t>(
                memWidth, std::numeric_limits<int32_t>::max()));
        auto applyWriteWidthHint = [&](ExprNodeId id) {
            if (memWidthHint <= 0 || id == kInvalidPlanIndex ||
                id >= static_cast<ExprNodeId>(lowering.values.size()))
            {
                return;
            }
            ExprNode& node = lowering.values[id];
            if (node.kind == ExprNodeKind::Symbol || node.kind == ExprNodeKind::XmrRead)
            {
                return;
            }
            if (node.widthHint <= 0 || node.widthHint < memWidthHint)
            {
                node.widthHint = memWidthHint;
            }
        };
        if (isPackedAggregateVariable(plan.signals[signal]) && write.slices.empty() &&
            write.target.valid() && write.target.index < indexedMemoryAccess.size() &&
            indexedMemoryAccess[write.target.index])
        {
            if (write.domain == ControlDomain::Sequential && stmt.eventEdges.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        write.location,
                        "Skipping memory fill without edge-sensitive timing control");
                }
                continue;
            }
            MemoryFillPort fill;
            fill.memory = write.target;
            fill.signal = signal;
            const int32_t fillWidthHint =
                static_cast<int32_t>(std::min<int64_t>(
                    aggregateWidth > 0 ? aggregateWidth : memWidth,
                    std::numeric_limits<int32_t>::max()));
            fill.data = builder.resizeValueToWidth(write.value, fillWidthHint,
                                                   plan.signals[signal].isSigned,
                                                   write.location);
            if (fill.data == kInvalidPlanIndex)
            {
                continue;
            }
            applyWriteWidthHint(fill.data);
            fill.updateCond = builder.ensureGuardExpr(write.guard, write.location);
            fill.eventEdges = stmt.eventEdges;
            fill.eventOperands = stmt.eventOperands;
            fill.location = write.location;
            lowering.memoryFills.push_back(std::move(fill));
            continue;
        }
        if (isPackedAggregateVariable(plan.signals[signal]) && write.slices.empty())
        {
            continue;
        }
        const auto& dims = plan.signals[signal].unpackedDims;
        if (write.slices.size() < (dims.empty() ? 1 : dims.size()))
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(write.location,
                                           "Memory write missing address slices");
            }
            continue;
        }

        std::vector<ExprNodeId> addressIndices;
        const std::size_t addressCount = dims.empty() ? 1 : dims.size();
        bool addressOk = true;
        for (std::size_t i = 0; i < addressCount; ++i)
        {
            const WriteSlice& addrSlice = write.slices[i];
            if (addrSlice.kind != WriteSliceKind::BitSelect ||
                addrSlice.index == kInvalidPlanIndex)
            {
                addressOk = false;
                break;
            }
            addressIndices.push_back(addrSlice.index);
        }
        if (!addressOk)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(write.location,
                                           "Unsupported memory address slice kind");
            }
            continue;
        }
        ExprNodeId address = kInvalidPlanIndex;
        if (dims.empty())
        {
            address = addressIndices.front();
        }
        else
        {
            address = buildLinearAddress(addressIndices, dims, write.location);
        }
        if (address == kInvalidPlanIndex)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(write.location,
                                           "Memory write address linearization failed");
            }
            continue;
        }
        ExprNodeId data = write.value;
        ExprNodeId mask = kInvalidPlanIndex;
        bool isMasked = false;
        const std::size_t packedStart = addressCount;
        if (write.slices.size() == packedStart)
        {
            auto literal = makeMaskLiteral(memWidth, 0, memWidth - 1);
            if (!literal)
            {
                continue;
            }
            mask = builder.addConstantLiteral(*literal, write.location);
        }
        else if (write.slices.size() == packedStart + 1)
        {
            const WriteSlice& dataSlice = write.slices[packedStart];
            if (dataSlice.kind == WriteSliceKind::BitSelect)
            {
                isMasked = true;
                const int32_t widthHint =
                    memWidth > 0
                        ? static_cast<int32_t>(std::min<int64_t>(
                              memWidth, std::numeric_limits<int32_t>::max()))
                        : 0;
                const ExprNodeId bitIndex = dataSlice.index;
                if (bitIndex == kInvalidPlanIndex)
                {
                    continue;
                }
                const std::string oneLiteral = std::to_string(memWidth) + "'b1";
                ExprNodeId one = builder.addConstantLiteral(oneLiteral, write.location);
                mask = builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kShl,
                                                      {one, bitIndex}, widthHint,
                                                      write.location);
                if (bitIndex == kInvalidPlanIndex)
                {
                    continue;
                }
                data = builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kShl,
                                                      {write.value, bitIndex},
                                                      widthHint, write.location);
            }
            else if (dataSlice.kind == WriteSliceKind::RangeSelect)
            {
                isMasked = true;
                const int32_t widthHint =
                    memWidth > 0
                        ? static_cast<int32_t>(std::min<int64_t>(
                              memWidth, std::numeric_limits<int32_t>::max()))
                        : 0;
                auto leftConst = evalConstInt(plan, lowering, dataSlice.left);
                auto rightConst = evalConstInt(plan, lowering, dataSlice.right);
                if (dataSlice.rangeKind == WriteRangeKind::Simple)
                {
                    if (!leftConst || !rightConst)
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                write.location,
                                "Memory range mask bounds must be constant");
                        }
                        continue;
                    }
                    int64_t lo = std::min(*leftConst, *rightConst);
                    int64_t hi = std::max(*leftConst, *rightConst);
                    if (lo < 0 || hi < lo || hi >= memWidth)
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                write.location,
                                "Memory range mask exceeds memory width");
                        }
                        continue;
                    }
                    auto literal = makeMaskLiteral(memWidth, lo, hi);
                    if (!literal)
                    {
                        continue;
                    }
                    mask = builder.addConstantLiteral(*literal, write.location);
                    if (lo != 0)
                    {
                        ExprNodeId shift =
                            builder.addConstantLiteral(std::to_string(lo), write.location);
                        data = builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kShl,
                                                              {write.value, shift},
                                                              widthHint, write.location);
                    }
                    else
                    {
                        data = write.value;
                    }
                }
                else if (dataSlice.rangeKind == WriteRangeKind::IndexedUp ||
                         dataSlice.rangeKind == WriteRangeKind::IndexedDown)
                {
                    ExprNodeId base = dataSlice.left;
                    ExprNodeId width = dataSlice.right;
                    if (base == kInvalidPlanIndex || width == kInvalidPlanIndex)
                    {
                        continue;
                    }
                    auto widthConst = evalConstInt(plan, lowering, width);
                    if (!widthConst)
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                write.location,
                                "Indexed part-select width must be constant");
                        }
                        continue;
                    }
                    if (*widthConst <= 0)
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                write.location,
                                "Indexed part-select width must be positive");
                        }
                        continue;
                    }
                    if (*widthConst > memWidth)
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                write.location,
                                "Indexed part-select exceeds memory width");
                        }
                        continue;
                    }
                    auto baseConst = evalConstInt(plan, lowering, base);
                    if (baseConst)
                    {
                        int64_t lo = 0;
                        int64_t hi = 0;
                        if (dataSlice.rangeKind == WriteRangeKind::IndexedUp)
                        {
                            lo = *baseConst;
                            hi = *baseConst + *widthConst - 1;
                        }
                        else
                        {
                            lo = *baseConst - *widthConst + 1;
                            hi = *baseConst;
                        }
                        if (lo < 0 || hi < lo || hi >= memWidth)
                        {
                            if (context_.diagnostics)
                            {
                                context_.diagnostics->warn(
                                    write.location,
                                    "Indexed part-select exceeds memory width");
                            }
                            continue;
                        }
                    }
                    else
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                write.location,
                                "Indexed part-select base is dynamic; bounds check skipped");
                        }
                    }
                    ExprNodeId widthLiteral =
                        builder.addConstantLiteral(std::to_string(*widthConst), write.location);
                    const std::string oneLiteral = std::to_string(memWidth) + "'b1";
                    ExprNodeId one = builder.addConstantLiteral(oneLiteral, write.location);
                    ExprNodeId shifted = builder.makeOperation(wolvrix::lib::grh::OperationKind::kShl,
                                                               {one, widthLiteral}, write.location);
                    ExprNodeId ones =
                        builder.makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                              {shifted, one}, write.location);
                    ExprNodeId shift = base;
                    if (dataSlice.rangeKind == WriteRangeKind::IndexedDown)
                    {
                        ExprNodeId baseMinus =
                            builder.makeOperation(wolvrix::lib::grh::OperationKind::kSub,
                                                  {base, widthLiteral}, write.location);
                        shift = builder.makeOperation(wolvrix::lib::grh::OperationKind::kAdd,
                                                      {baseMinus, one}, write.location);
                    }
                    mask = builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kShl,
                                                          {ones, shift}, widthHint,
                                                          write.location);
                    data = builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kShl,
                                                          {write.value, shift},
                                                          widthHint, write.location);
                }
                else
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->error(write.location,
                                                   "Dynamic memory range mask is unsupported");
                    }
                    continue;
                }
            }
            else
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->error(write.location,
                                               "Unsupported memory write slice kind");
                }
                continue;
            }
        }
        else
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->error(write.location,
                                           "Multi-slice memory write is unsupported");
            }
            continue;
        }

        if (mask == kInvalidPlanIndex || data == kInvalidPlanIndex)
        {
            continue;
        }

        applyWriteWidthHint(data);
        applyWriteWidthHint(mask);

        if (write.domain == ControlDomain::Sequential && stmt.eventEdges.empty())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(
                    write.location,
                    "Skipping memory write without edge-sensitive timing control");
            }
            continue;
        }

        MemoryWritePort entry;
        entry.memory = write.target;
        entry.signal = signal;
        entry.address = address;
        entry.data = data;
        entry.mask = mask;
        entry.updateCond = builder.ensureGuardExpr(write.guard, write.location);
        entry.isMasked = isMasked;
        entry.eventEdges = stmt.eventEdges;
        entry.eventOperands = stmt.eventOperands;
        entry.location = write.location;
        lowering.memoryWrites.push_back(std::move(entry));
    }

    auto exprStructurallyEquivalent = [&](auto&& self, ExprNodeId lhs, ExprNodeId rhs) -> bool {
        if (lhs == rhs)
        {
            return true;
        }
        if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex ||
            lhs >= lowering.values.size() || rhs >= lowering.values.size())
        {
            return false;
        }
        const ExprNode& lhsNode = lowering.values[lhs];
        const ExprNode& rhsNode = lowering.values[rhs];
        if (lhsNode.kind != rhsNode.kind)
        {
            return false;
        }
        switch (lhsNode.kind)
        {
        case ExprNodeKind::Invalid:
            return true;
        case ExprNodeKind::Constant:
            return lhsNode.literal == rhsNode.literal;
        case ExprNodeKind::Symbol:
            return lhsNode.symbol.index == rhsNode.symbol.index;
        case ExprNodeKind::XmrRead:
            return lhsNode.xmrPath == rhsNode.xmrPath;
        case ExprNodeKind::Operation:
            if (lhsNode.op != rhsNode.op ||
                lhsNode.operands.size() != rhsNode.operands.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhsNode.operands.size(); ++i)
            {
                if (!self(self, lhsNode.operands[i], rhsNode.operands[i]))
                {
                    return false;
                }
            }
            return true;
        }
        return false;
    };

    auto exprListStructurallyEquivalent =
        [&](const std::vector<ExprNodeId>& lhs,
            const std::vector<ExprNodeId>& rhs) -> bool {
        if (lhs.size() != rhs.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i)
        {
            if (!exprStructurallyEquivalent(exprStructurallyEquivalent, lhs[i], rhs[i]))
            {
                return false;
            }
        }
        return true;
    };

    auto makeLogicAnd = [&](ExprNodeId lhs, ExprNodeId rhs,
                            slang::SourceLocation location) -> ExprNodeId {
        if (lhs == kInvalidPlanIndex)
        {
            return rhs;
        }
        if (rhs == kInvalidPlanIndex)
        {
            return lhs;
        }
        return builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kLogicAnd,
                                              {lhs, rhs}, 1, location);
    };

    auto makeLogicOr = [&](ExprNodeId lhs, ExprNodeId rhs,
                           slang::SourceLocation location) -> ExprNodeId {
        if (lhs == kInvalidPlanIndex)
        {
            return rhs;
        }
        if (rhs == kInvalidPlanIndex)
        {
            return lhs;
        }
        return builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kLogicOr,
                                              {lhs, rhs}, 1, location);
    };

    auto makeLogicNot = [&](ExprNodeId operand,
                            slang::SourceLocation location) -> ExprNodeId {
        if (operand == kInvalidPlanIndex)
        {
            return kInvalidPlanIndex;
        }
        return builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kLogicNot,
                                              {operand}, 1, location);
    };

    auto exprWidthHint = [&](ExprNodeId id) -> int32_t {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return 0;
        }
        return lowering.values[id].widthHint;
    };

    auto flattenAndTerms = [&](auto&& self, ExprNodeId id,
                               std::vector<ExprNodeId>& terms) -> void {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return;
        }
        const ExprNode& node = lowering.values[id];
        if (node.kind == ExprNodeKind::Operation &&
            (node.op == wolvrix::lib::grh::OperationKind::kLogicAnd ||
             node.op == wolvrix::lib::grh::OperationKind::kAnd) &&
            node.operands.size() == 2)
        {
            self(self, node.operands[0], terms);
            self(self, node.operands[1], terms);
            return;
        }
        terms.push_back(id);
    };

    auto matchEqualityToConst =
        [&](ExprNodeId id, int64_t addressConst, ExprNodeId& dynamicAddress) -> bool {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return false;
        }
        const ExprNode& node = lowering.values[id];
        if (node.kind != ExprNodeKind::Operation)
        {
            return false;
        }
        if ((node.op == wolvrix::lib::grh::OperationKind::kEq ||
             node.op == wolvrix::lib::grh::OperationKind::kCaseEq) &&
            node.operands.size() == 2)
        {
            auto lhsConst = evalConstInt(plan, lowering, node.operands[0]);
            auto rhsConst = evalConstInt(plan, lowering, node.operands[1]);
            if (lhsConst && *lhsConst == addressConst && !rhsConst)
            {
                dynamicAddress = node.operands[1];
                return true;
            }
            if (rhsConst && *rhsConst == addressConst && !lhsConst)
            {
                dynamicAddress = node.operands[0];
                return true;
            }
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kReduceAnd &&
            node.operands.size() == 1)
        {
            ExprNodeId operand = node.operands[0];
            if (operand != kInvalidPlanIndex && operand < lowering.values.size() &&
                addressConst > 0 && ((addressConst + 1) & addressConst) == 0)
            {
                dynamicAddress = operand;
                return true;
            }
        }
        return false;
    };

    std::vector<ExprNodeId> singleAssignNetValue(plan.symbolTable.size(),
                                                 kInvalidPlanIndex);
    std::vector<uint8_t> singleAssignNetAmbiguous(plan.symbolTable.size(), 0);
    for (const auto& stmt : lowering.loweredStmts)
    {
        if (stmt.kind != LoweredStmtKind::Write || stmt.write.isXmr ||
            stmt.write.domain != ControlDomain::Combinational ||
            !stmt.eventEdges.empty() || !stmt.write.slices.empty() ||
            !stmt.write.target.valid() ||
            stmt.write.target.index >= singleAssignNetValue.size())
        {
            continue;
        }
        SignalId signal = resolveMemorySignal(stmt.write.target);
        if (signal != kInvalidPlanIndex ||
            stmt.write.target.index >= signalBySymbol.size())
        {
            continue;
        }
        SignalId targetSignal = signalBySymbol[stmt.write.target.index];
        if (targetSignal == kInvalidPlanIndex ||
            targetSignal >= static_cast<SignalId>(plan.signals.size()) ||
            plan.signals[targetSignal].kind != SignalKind::Net)
        {
            continue;
        }
        ExprNodeId& slot = singleAssignNetValue[stmt.write.target.index];
        if (slot == kInvalidPlanIndex)
        {
            slot = stmt.write.value;
        }
        else
        {
            singleAssignNetAmbiguous[stmt.write.target.index] = 1;
        }
    }

    auto resolveSingleAssignNet =
        [&](auto&& self, ExprNodeId id, std::vector<uint8_t>& seen) -> ExprNodeId {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return id;
        }
        const ExprNode& node = lowering.values[id];
        if (node.kind != ExprNodeKind::Symbol || !node.symbol.valid() ||
            node.symbol.index >= singleAssignNetValue.size() ||
            singleAssignNetAmbiguous[node.symbol.index] ||
            singleAssignNetValue[node.symbol.index] == kInvalidPlanIndex)
        {
            return id;
        }
        if (seen[node.symbol.index])
        {
            return id;
        }
        seen[node.symbol.index] = 1;
        return self(self, singleAssignNetValue[node.symbol.index], seen);
    };

    auto matchResolvedEqualityToConst =
        [&](ExprNodeId id, int64_t addressConst, ExprNodeId& dynamicAddress) -> bool {
        if (matchEqualityToConst(id, addressConst, dynamicAddress))
        {
            return true;
        }
        std::vector<uint8_t> seen(plan.symbolTable.size(), 0);
        ExprNodeId resolved = resolveSingleAssignNet(resolveSingleAssignNet, id, seen);
        return resolved != id && matchEqualityToConst(resolved, addressConst, dynamicAddress);
    };

    auto makeAddressEquality = [&](ExprNodeId lhs, ExprNodeId rhs,
                                   slang::SourceLocation location) -> ExprNodeId {
        return builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kEq,
                                              {lhs, rhs}, 1, location);
    };

    auto replaceAddressConstMatch =
        [&](auto&& self, ExprNodeId id, int64_t addressConst, ExprNodeId newAddress,
            ExprNodeId& replaced) -> ExprNodeId {
        if (id == kInvalidPlanIndex || id >= lowering.values.size())
        {
            return kInvalidPlanIndex;
        }
        const ExprNode& node = lowering.values[id];
        ExprNodeId dynamicAddress = kInvalidPlanIndex;
        if (matchResolvedEqualityToConst(id, addressConst, dynamicAddress))
        {
            replaced = dynamicAddress;
            return makeAddressEquality(dynamicAddress, newAddress, node.location);
        }
        if (node.kind != ExprNodeKind::Operation)
        {
            return id;
        }
        std::vector<ExprNodeId> operands;
        operands.reserve(node.operands.size());
        bool changed = false;
        for (ExprNodeId operand : node.operands)
        {
            ExprNodeId before = replaced;
            ExprNodeId rewritten = self(self, operand, addressConst, newAddress, replaced);
            if (rewritten == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            changed = changed || rewritten != operand || before != replaced;
            operands.push_back(rewritten);
        }
        if (!changed)
        {
            return id;
        }
        return builder.makeOperationWithWidth(node.op, std::move(operands),
                                              node.widthHint, node.location);
    };

    auto matchIndexedWriteGuard =
        [&](ExprNodeId guard, int64_t addressConst, ExprNodeId& dynamicAddress,
            std::vector<ExprNodeId>& enableTerms) -> bool {
        std::vector<ExprNodeId> terms;
        flattenAndTerms(flattenAndTerms, guard, terms);
        bool found = false;
        enableTerms.clear();
        for (ExprNodeId term : terms)
        {
            ExprNodeId termDynamicAddress = kInvalidPlanIndex;
            if (!found && matchResolvedEqualityToConst(term, addressConst, termDynamicAddress))
            {
                dynamicAddress = termDynamicAddress;
                found = true;
                continue;
            }
            enableTerms.push_back(term);
        }
        return found;
    };

    auto exprTermExists =
        [&](const std::vector<ExprNodeId>& terms, ExprNodeId candidate) -> bool {
        for (ExprNodeId term : terms)
        {
            if (exprStructurallyEquivalent(exprStructurallyEquivalent, term, candidate))
            {
                return true;
            }
        }
        return false;
    };

    auto sortedCandidatesByAddress =
        [](std::vector<IndexedMemoryWriteMergeCandidate> candidates) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const IndexedMemoryWriteMergeCandidate& lhs,
                     const IndexedMemoryWriteMergeCandidate& rhs) {
                      return lhs.addressConst < rhs.addressConst;
                  });
        return candidates;
    };

    auto commonEnableTerms =
        [&](const std::vector<IndexedMemoryWriteMergeCandidate>& candidates) {
        std::vector<ExprNodeId> common;
        if (candidates.empty())
        {
            return common;
        }
        for (ExprNodeId term : candidates.front().enableTerms)
        {
            bool presentInAll = true;
            for (std::size_t i = 1; i < candidates.size(); ++i)
            {
                if (!exprTermExists(candidates[i].enableTerms, term))
                {
                    presentInAll = false;
                    break;
                }
            }
            if (presentInAll && !exprTermExists(common, term))
            {
                common.push_back(term);
            }
        }
        return common;
    };

    auto buildPriorityConflictExpr =
        [&](const std::vector<IndexedMemoryWriteMergeCandidate>& candidates,
            const std::vector<ExprNodeId>& commonTerms) -> ExprNodeId {
        ExprNodeId conflictExpr = kInvalidPlanIndex;
        ExprNodeId blockedDynamicAddress = kInvalidPlanIndex;
        for (const auto& candidate : candidates)
        {
            bool foundForCandidate = false;
            for (ExprNodeId term : candidate.enableTerms)
            {
                if (exprTermExists(commonTerms, term))
                {
                    continue;
                }
                if (term == kInvalidPlanIndex || term >= lowering.values.size())
                {
                    continue;
                }
                const ExprNode& termNode = lowering.values[term];
                if (termNode.kind != ExprNodeKind::Operation ||
                    termNode.op != wolvrix::lib::grh::OperationKind::kLogicNot ||
                    termNode.operands.size() != 1)
                {
                    continue;
                }
                ExprNodeId termDynamicAddress = kInvalidPlanIndex;
                ExprNodeId rewritten =
                    replaceAddressConstMatch(replaceAddressConstMatch,
                                             termNode.operands[0],
                                             candidate.addressConst,
                                             candidate.dynamicAddress,
                                             termDynamicAddress);
                if (termDynamicAddress == kInvalidPlanIndex ||
                    rewritten == kInvalidPlanIndex ||
                    rewritten == termNode.operands[0])
                {
                    continue;
                }
                if (blockedDynamicAddress == kInvalidPlanIndex)
                {
                    blockedDynamicAddress = termDynamicAddress;
                }
                else if (!exprStructurallyEquivalent(exprStructurallyEquivalent,
                                                     blockedDynamicAddress,
                                                     termDynamicAddress))
                {
                    return kInvalidPlanIndex;
                }
                if (conflictExpr == kInvalidPlanIndex)
                {
                    conflictExpr = rewritten;
                }
                else if (!exprStructurallyEquivalent(exprStructurallyEquivalent,
                                                     conflictExpr,
                                                     rewritten))
                {
                    return kInvalidPlanIndex;
                }
                foundForCandidate = true;
                break;
            }
            if (!foundForCandidate)
            {
                return kInvalidPlanIndex;
            }
        }
        return conflictExpr;
    };

    auto buildEnableFromTerms =
        [&](const std::vector<ExprNodeId>& terms, slang::SourceLocation location) {
        ExprNodeId enable = kInvalidPlanIndex;
        for (ExprNodeId term : terms)
        {
            enable = makeLogicAnd(enable, term, location);
        }
        return enable == kInvalidPlanIndex
                   ? builder.ensureGuardExpr(kInvalidPlanIndex, location)
                   : enable;
    };

    auto mergeSameAddressMemoryWrites = [&]() {
        if (lowering.memoryWrites.size() < 2)
        {
            return;
        }
        std::vector<uint8_t> consumed(lowering.memoryWrites.size(), 0);
        std::vector<MemoryWritePort> merged;
        merged.reserve(lowering.memoryWrites.size());
        for (std::size_t i = 0; i < lowering.memoryWrites.size(); ++i)
        {
            if (consumed[i])
            {
                continue;
            }
            const MemoryWritePort& seed = lowering.memoryWrites[i];
            std::vector<std::size_t> group;
            group.push_back(i);
            for (std::size_t j = i + 1; j < lowering.memoryWrites.size(); ++j)
            {
                if (consumed[j])
                {
                    continue;
                }
                const MemoryWritePort& other = lowering.memoryWrites[j];
                if (other.memory.index != seed.memory.index ||
                    other.signal != seed.signal ||
                    other.isMasked != seed.isMasked ||
                    other.eventEdges != seed.eventEdges ||
                    !exprListStructurallyEquivalent(other.eventOperands, seed.eventOperands))
                {
                    continue;
                }
                if (!exprStructurallyEquivalent(exprStructurallyEquivalent,
                                                other.address,
                                                seed.address) ||
                    !exprStructurallyEquivalent(exprStructurallyEquivalent,
                                                other.mask,
                                                seed.mask))
                {
                    continue;
                }
                group.push_back(j);
            }
            if (group.size() == 1)
            {
                merged.push_back(seed);
                consumed[i] = 1;
                continue;
            }

            MemoryWritePort combined = seed;
            ExprNodeId updateCond = kInvalidPlanIndex;
            for (std::size_t index : group)
            {
                const MemoryWritePort& write = lowering.memoryWrites[index];
                updateCond = makeLogicOr(updateCond, write.updateCond, write.location);
            }
            ExprNodeId data = lowering.memoryWrites[group.front()].data;
            for (std::size_t idx = 1; idx < group.size(); ++idx)
            {
                const MemoryWritePort& write = lowering.memoryWrites[group[idx]];
                int32_t widthHint = exprWidthHint(write.data);
                if (widthHint <= 0)
                {
                    widthHint = exprWidthHint(data);
                }
                data = builder.makeOperationWithWidth(wolvrix::lib::grh::OperationKind::kMux,
                                                      {write.updateCond, write.data, data},
                                                      widthHint, write.location);
            }
            combined.updateCond = updateCond == kInvalidPlanIndex
                                      ? builder.ensureGuardExpr(kInvalidPlanIndex,
                                                                seed.location)
                                      : updateCond;
            combined.data = data;
            combined.location = lowering.memoryWrites[group.back()].location;
            for (std::size_t index : group)
            {
                consumed[index] = 1;
            }
            merged.push_back(std::move(combined));
        }
        lowering.memoryWrites = std::move(merged);
    };

    auto mergeIndexedMemoryWrites = [&]() {
        if (lowering.memoryWrites.size() < 4)
        {
            return;
        }
        std::vector<uint8_t> consumed(lowering.memoryWrites.size(), 0);
        std::vector<MemoryWritePort> merged;
        merged.reserve(lowering.memoryWrites.size());
        for (std::size_t i = 0; i < lowering.memoryWrites.size(); ++i)
        {
            if (consumed[i])
            {
                continue;
            }
            MemoryWritePort& seed = lowering.memoryWrites[i];
            auto seedAddrConst = evalConstInt(plan, lowering, seed.address);
            if (!seedAddrConst)
            {
                merged.push_back(seed);
                consumed[i] = 1;
                continue;
            }
            ExprNodeId seedDynamicAddress = kInvalidPlanIndex;
            std::vector<ExprNodeId> seedEnableTerms;
            if (!matchIndexedWriteGuard(seed.updateCond,
                                        *seedAddrConst,
                                        seedDynamicAddress,
                                        seedEnableTerms))
            {
                merged.push_back(seed);
                consumed[i] = 1;
                continue;
            }
            std::vector<IndexedMemoryWriteMergeCandidate> candidates;
            candidates.push_back(IndexedMemoryWriteMergeCandidate{
                .writeIndex = i,
                .addressConst = *seedAddrConst,
                .dynamicAddress = seedDynamicAddress,
                .enableTerms = seedEnableTerms,
            });
            for (std::size_t j = i + 1; j < lowering.memoryWrites.size(); ++j)
            {
                if (consumed[j])
                {
                    continue;
                }
                const MemoryWritePort& other = lowering.memoryWrites[j];
                if (other.memory.index != seed.memory.index ||
                    other.signal != seed.signal ||
                    other.isMasked != seed.isMasked ||
                    other.eventEdges != seed.eventEdges ||
                    !exprListStructurallyEquivalent(other.eventOperands, seed.eventOperands))
                {
                    continue;
                }
                if (!exprStructurallyEquivalent(exprStructurallyEquivalent, other.data, seed.data))
                {
                    continue;
                }
                if (!exprStructurallyEquivalent(exprStructurallyEquivalent, other.mask, seed.mask))
                {
                    continue;
                }
                auto otherAddrConst = evalConstInt(plan, lowering, other.address);
                if (!otherAddrConst)
                {
                    continue;
                }
                ExprNodeId otherDynamicAddress = kInvalidPlanIndex;
                std::vector<ExprNodeId> otherEnableTerms;
                if (!matchIndexedWriteGuard(other.updateCond,
                                            *otherAddrConst,
                                            otherDynamicAddress,
                                            otherEnableTerms))
                {
                    continue;
                }
                if (!exprStructurallyEquivalent(exprStructurallyEquivalent,
                                                otherDynamicAddress,
                                                seedDynamicAddress))
                {
                    continue;
                }
                candidates.push_back(IndexedMemoryWriteMergeCandidate{
                    .writeIndex = j,
                    .addressConst = *otherAddrConst,
                    .dynamicAddress = otherDynamicAddress,
                    .enableTerms = otherEnableTerms,
                });
            }
            if (candidates.size() < 8)
            {
                merged.push_back(seed);
                consumed[i] = 1;
                continue;
            }
            std::vector<IndexedMemoryWriteMergeCandidate> sorted =
                sortedCandidatesByAddress(candidates);
            bool coversContiguousRange = true;
            for (std::size_t idx = 0; idx < sorted.size(); ++idx)
            {
                if (sorted[idx].addressConst != static_cast<int64_t>(idx))
                {
                    coversContiguousRange = false;
                    break;
                }
            }
            if (!coversContiguousRange)
            {
                merged.push_back(seed);
                consumed[i] = 1;
                continue;
            }
            std::vector<ExprNodeId> enableTerms = commonEnableTerms(candidates);
            ExprNodeId priorityConflict =
                buildPriorityConflictExpr(candidates, enableTerms);
            if (priorityConflict != kInvalidPlanIndex)
            {
                ExprNodeId notConflict = makeLogicNot(priorityConflict, seed.location);
                if (notConflict != kInvalidPlanIndex)
                {
                    enableTerms.push_back(notConflict);
                }
            }
            if (enableTerms.empty())
            {
                merged.push_back(seed);
                consumed[i] = 1;
                continue;
            }
            MemoryWritePort combined = seed;
            combined.address = seedDynamicAddress;
            combined.updateCond = buildEnableFromTerms(enableTerms, seed.location);
            for (const auto& candidate : candidates)
            {
                consumed[candidate.writeIndex] = 1;
            }
            merged.push_back(std::move(combined));
        }
        lowering.memoryWrites = std::move(merged);
    };

    mergeSameAddressMemoryWrites();
    mergeIndexedMemoryWrites();

    if (!lowering.memoryReads.empty() && !lowering.memoryWrites.empty())
    {
        for (auto& read : lowering.memoryReads)
        {
            if (!read.isSync || read.data == kInvalidPlanIndex)
            {
                continue;
            }
            for (const auto& write : lowering.memoryWrites)
            {
                if (write.memory.index != read.memory.index)
                {
                    continue;
                }
                if (exprDependsOn(write.data, read.data) ||
                    exprDependsOn(write.updateCond, read.data))
                {
                    read.isSync = false;
                    read.updateCond = kInvalidPlanIndex;
                    read.eventEdges.clear();
                    read.eventOperands.clear();
                    break;
                }
            }
        }
    }
}

class GraphAssemblyState {
public:
    GraphAssemblyState(ConvertContext& context, GraphAssembler& assembler, wolvrix::lib::grh::Graph& graph,
                       const ModulePlan& plan, LoweringPlan& lowering,
                       const WriteBackPlan& writeBack)
        : context_(context), assembler_(assembler), graph_(graph), plan_(plan),
          lowering_(lowering), writeBack_(writeBack),
          sourceManager_(context.compilation ? context.compilation->getSourceManager() : nullptr),
          connectionLowerer_(*this)
    {
        symbolIds_.assign(plan_.symbolTable.size(), wolvrix::lib::grh::SymbolId::invalid());
        valueBySymbol_.assign(plan_.symbolTable.size(), wolvrix::lib::grh::ValueId::invalid());
        valueByExpr_.assign(lowering_.values.size(), wolvrix::lib::grh::ValueId::invalid());
        packedAggregateWholeReadBySymbol_.assign(plan_.symbolTable.size(), wolvrix::lib::grh::ValueId::invalid());
        memoryOpBySymbol_.assign(plan_.symbolTable.size(), wolvrix::lib::grh::OperationId::invalid());
        memorySymbolName_.assign(plan_.symbolTable.size(), std::string());
        storageBySymbol_.assign(plan_.symbolTable.size(), StorageKind::None);
        storageOpBySymbol_.assign(plan_.symbolTable.size(), wolvrix::lib::grh::OperationId::invalid());
        storageLocation_.assign(plan_.symbolTable.size(), slang::SourceLocation{});
        memoryReadIndexByExpr_.assign(lowering_.values.size(), kInvalidMemoryReadIndex);
        for (std::size_t i = 0; i < lowering_.memoryReads.size(); ++i)
        {
            ExprNodeId data = lowering_.memoryReads[i].replacement;
            if (data == kInvalidPlanIndex)
            {
                data = lowering_.memoryReads[i].data;
            }
            if (data == kInvalidPlanIndex || data >= memoryReadIndexByExpr_.size())
            {
                continue;
            }
            if (memoryReadIndexByExpr_[data] == kInvalidMemoryReadIndex)
            {
                memoryReadIndexByExpr_[data] = static_cast<int32_t>(i);
            }
        }
        initStorageKinds();
    }

    void build()
    {
        preloadPlanSymbols();
        createStorageDeclarations();
        createPortValues();
        createSignalValues();
        createInoutSignalValues();
        emitGenerateGroups();
        createMemoryOps();
        emitMemoryPorts();
        emitSideEffects();
        emitInstances();
        emitInstanceSliceWrites();
        emitXmrWrites();
        emitWriteBack();
    }

private:
    static int32_t normalizeWidth(int32_t width) { return width > 0 ? width : 1; }
    static constexpr int32_t kInvalidMemoryReadIndex = -1;

    enum class StorageKind {
        None,
        Register,
        Latch
    };

    std::optional<wolvrix::lib::grh::SrcLoc> resolveSrcLoc(slang::SourceLocation location) const
    {
        if (!location.valid() || !sourceManager_)
        {
            return std::nullopt;
        }
        const auto loc = sourceManager_->getFullyOriginalLoc(location);
        if (!loc.valid() || !sourceManager_->isFileLoc(loc))
        {
            return std::nullopt;
        }
        wolvrix::lib::grh::SrcLoc out{};
        out.file = std::string(sourceManager_->getFileName(loc));
        out.line = static_cast<uint32_t>(sourceManager_->getLineNumber(loc));
        out.column = static_cast<uint32_t>(sourceManager_->getColumnNumber(loc));
        return out;
    }

    void preloadPlanSymbols()
    {
        const std::size_t count = plan_.symbolTable.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            PlanSymbolId id{static_cast<PlanIndex>(i)};
            std::string_view text = plan_.symbolTable.text(id);
            if (text.empty())
            {
                continue;
            }
            wolvrix::lib::grh::SymbolId sym = graph_.internSymbol(text);
            if (!sym.valid())
            {
                throw std::runtime_error("Plan symbol already bound to value/operation: " +
                                         std::string(text));
            }
            symbolIds_[i] = sym;
        }
    }

    void maybeSetOpSrcLoc(wolvrix::lib::grh::OperationId op, slang::SourceLocation location)
    {
        if (auto loc = resolveSrcLoc(location))
        {
            graph_.setOpSrcLoc(op, std::move(*loc));
        }
    }

    wolvrix::lib::grh::OperationId createOp(wolvrix::lib::grh::OperationKind kind, wolvrix::lib::grh::SymbolId symbol,
                                  slang::SourceLocation location)
    {
        if (!symbol.valid())
        {
            symbol = makeInternalOpSymbol();
        }
        wolvrix::lib::grh::OperationId op = graph_.createOperation(kind, symbol);
        maybeSetOpSrcLoc(op, location);
        return op;
    }

    wolvrix::lib::grh::ValueId createStaticSliceValue(wolvrix::lib::grh::ValueId base,
                                                      int64_t low,
                                                      int64_t high,
                                                      bool isSigned,
                                                      wolvrix::lib::grh::ValueType valueType,
                                                      slang::SourceLocation location)
    {
        if (!base.valid() || low < 0 || high < low)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }

        const int64_t width = high - low + 1;
        if (width <= 0 || width > std::numeric_limits<int32_t>::max())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }

        const wolvrix::lib::grh::Value baseValue = graph_.getValue(base);
        const int64_t baseWidth = baseValue.width();
        if (baseWidth > 0 && high >= baseWidth)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }

        const wolvrix::lib::grh::OperationId baseDefId = baseValue.definingOp();
        if (baseDefId.valid())
        {
            const wolvrix::lib::grh::Operation baseDef = graph_.getOperation(baseDefId);
            if (baseDef.kind() == wolvrix::lib::grh::OperationKind::kConcat)
            {
                const auto& concatOperands = baseDef.operands();
                int64_t totalWidth = 0;
                std::vector<int64_t> operandWidths;
                operandWidths.reserve(concatOperands.size());
                bool widthsOk = true;
                for (const auto operand : concatOperands)
                {
                    if (!operand.valid())
                    {
                        widthsOk = false;
                        break;
                    }
                    const int64_t operandWidth = graph_.getValue(operand).width();
                    if (operandWidth <= 0)
                    {
                        widthsOk = false;
                        break;
                    }
                    operandWidths.push_back(operandWidth);
                    totalWidth += operandWidth;
                }
                if (widthsOk && totalWidth > 0 && high < totalWidth)
                {
                    int64_t cursor = totalWidth;
                    for (std::size_t i = 0; i < concatOperands.size(); ++i)
                    {
                        const int64_t operandWidth = operandWidths[i];
                        const int64_t operandHigh = cursor - 1;
                        const int64_t operandLow = cursor - operandWidth;
                        cursor = operandLow;
                        if (low < operandLow || high > operandHigh)
                        {
                            continue;
                        }

                        const wolvrix::lib::grh::ValueId selected = concatOperands[i];
                        const wolvrix::lib::grh::Value selectedValue =
                            graph_.getValue(selected);
                        const int64_t newLow = low - operandLow;
                        const int64_t newHigh = high - operandLow;
                        if (newLow == 0 &&
                            newHigh + 1 == selectedValue.width() &&
                            selectedValue.width() == width &&
                            selectedValue.isSigned() == isSigned &&
                            selectedValue.type() == valueType)
                        {
                            return selected;
                        }
                        return createStaticSliceValue(selected, newLow, newHigh,
                                                      isSigned, valueType, location);
                    }
                }
            }
        }

        wolvrix::lib::grh::OperationId op =
            createOp(wolvrix::lib::grh::OperationKind::kSliceStatic,
                     wolvrix::lib::grh::SymbolId::invalid(),
                     location);
        graph_.addOperand(op, base);
        graph_.setAttr(op, "sliceStart", low);
        graph_.setAttr(op, "sliceEnd", high);
        wolvrix::lib::grh::SymbolId sym = makeInternalValueSymbol();
        wolvrix::lib::grh::ValueId result =
            graph_.createValue(sym, static_cast<int32_t>(width),
                               isSigned, valueType);
        graph_.addResult(op, result);
        return result;
    }

    wolvrix::lib::grh::SymbolId symbolForPlan(PlanSymbolId id)
    {
        if (!id.valid() || id.index >= symbolIds_.size())
        {
            return wolvrix::lib::grh::SymbolId::invalid();
        }
        if (symbolIds_[id.index].valid())
        {
            return symbolIds_[id.index];
        }
        const std::string text(plan_.symbolTable.text(id));
        if (text.empty())
        {
            return wolvrix::lib::grh::SymbolId::invalid();
        }
        wolvrix::lib::grh::SymbolId sym = graph_.internSymbol(text);
        if (!sym.valid())
        {
            throw std::runtime_error("Plan symbol already bound to value/operation: " + text);
        }
        symbolIds_[id.index] = sym;
        return symbolIds_[id.index];
    }

    void markDeclaredSymbol(PlanSymbolId id)
    {
        wolvrix::lib::grh::SymbolId sym = symbolForPlan(id);
        if (sym.valid())
        {
            graph_.addDeclaredSymbol(sym);
        }
    }

    wolvrix::lib::grh::ValueId valueForSymbol(PlanSymbolId id)
    {
        if (!id.valid() || id.index >= valueBySymbol_.size())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (!valueBySymbol_[id.index].valid() && isMemoryBackedPackedAggregate(id))
        {
            return emitPackedAggregateWholeRead(id, slang::SourceLocation{});
        }
        if (!valueBySymbol_[id.index].valid() && isStorageSymbol(id))
        {
            return ensureStorageReadValue(id, storageBySymbol_[id.index],
                                          storageLocation_[id.index]);
        }
        return valueBySymbol_[id.index];
    }

    wolvrix::lib::grh::ValueId createValue(PlanSymbolId id, int32_t width, bool isSigned,
                                 wolvrix::lib::grh::ValueType type)
    {
        if (!id.valid() || id.index >= valueBySymbol_.size())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (valueBySymbol_[id.index].valid())
        {
            return valueBySymbol_[id.index];
        }
        wolvrix::lib::grh::SymbolId symbol = symbolForPlan(id);
        if (!symbol.valid())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        const int32_t normalized = normalizeWidth(width);
        wolvrix::lib::grh::ValueId value = graph_.createValue(symbol, normalized, isSigned, type);
        valueBySymbol_[id.index] = value;
        return value;
    }

    struct StorageInfo {
        int32_t width = 0;
        bool isSigned = false;
        wolvrix::lib::grh::ValueType valueType = wolvrix::lib::grh::ValueType::Logic;
    };

    bool isStorageSymbol(PlanSymbolId id) const
    {
        if (!id.valid() || id.index >= storageBySymbol_.size())
        {
            return false;
        }
        return storageBySymbol_[id.index] != StorageKind::None;
    }

    std::optional<StorageInfo> resolveStorageInfo(PlanSymbolId id) const
    {
        StorageInfo info{};
        if (!id.valid())
        {
            return std::nullopt;
        }
        if (const SignalInfo* signal = findSignalInfo(id))
        {
            int32_t width = signal->width;
            if (isFlattenedNetArray(*signal))
            {
                width = static_cast<int32_t>(flattenedNetWidth(*signal));
            }
            else if (isPackedAggregateVariable(*signal))
            {
                width = static_cast<int32_t>(flattenedAggregateWidth(*signal));
            }
            info.width = normalizeWidth(width);
            info.isSigned = signal->isSigned;
            info.valueType = signal->valueType;
            return info;
        }
        std::string_view name = plan_.symbolTable.text(id);
        if (!name.empty())
        {
            if (const PortInfo* port = findPortByName(plan_, name))
            {
                info.width = normalizeWidth(port->width);
                info.isSigned = port->isSigned;
                info.valueType = port->valueType;
                return info;
            }
        }
        return std::nullopt;
    }

    wolvrix::lib::grh::ValueId ensureStorageReadValue(PlanSymbolId id,
                                            StorageKind kind,
                                            slang::SourceLocation location)
    {
        if (!id.valid() || id.index >= valueBySymbol_.size())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (valueBySymbol_[id.index].valid())
        {
            return valueBySymbol_[id.index];
        }
        const auto info = resolveStorageInfo(id);
        if (!info)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(location,
                                           "Skipping storage read port without width info");
            }
            return wolvrix::lib::grh::ValueId::invalid();
        }
        std::string_view name = plan_.symbolTable.text(id);
        if (name.empty())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        const bool isRegister = kind == StorageKind::Register;
        wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol();
        wolvrix::lib::grh::OperationId op =
            createOp(isRegister ? wolvrix::lib::grh::OperationKind::kRegisterReadPort
                                : wolvrix::lib::grh::OperationKind::kLatchReadPort,
                     opSym, location);
        wolvrix::lib::grh::SymbolId valSym = makeInternalValueSymbol();
        wolvrix::lib::grh::ValueId value =
            graph_.createValue(valSym, info->width, info->isSigned, info->valueType);
        graph_.addResult(op, value);
        if (isRegister)
        {
            graph_.setAttr(op, "regSymbol", std::string(name));
        }
        else
        {
            graph_.setAttr(op, "latchSymbol", std::string(name));
        }
        valueBySymbol_[id.index] = value;
        return value;
    }

    void initStorageKinds()
    {
        if (storageBySymbol_.empty())
        {
            return;
        }
        for (const auto& entry : writeBack_.entries)
        {
            if (!entry.target.valid() || entry.target.index >= storageBySymbol_.size())
            {
                continue;
            }
            StorageKind kind = StorageKind::None;
            if (entry.domain == ControlDomain::Sequential)
            {
                kind = StorageKind::Register;
            }
            else if (entry.domain == ControlDomain::Latch)
            {
                kind = StorageKind::Latch;
            }
            if (kind == StorageKind::None)
            {
                continue;
            }
            StorageKind& current = storageBySymbol_[entry.target.index];
            if (current == StorageKind::None)
            {
                current = kind;
                if (entry.location.valid())
                {
                    storageLocation_[entry.target.index] = entry.location;
                }
                continue;
            }
            if (current != kind && context_.diagnostics)
            {
                context_.diagnostics->warn(
                    entry.location,
                    "Storage target written in mixed sequential/latch domains");
            }
        }

        for (const auto& init : lowering_.registerInits)
        {
            if (!init.reg.valid() || init.reg.index >= storageBySymbol_.size())
            {
                continue;
            }

            StorageKind& current = storageBySymbol_[init.reg.index];
            if (current == StorageKind::Register)
            {
                if (!storageLocation_[init.reg.index].valid() && init.location.valid())
                {
                    storageLocation_[init.reg.index] = init.location;
                }
                continue;
            }
            if (current == StorageKind::Latch)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        init.location,
                        "Skipping register initializer for latch storage target");
                }
                continue;
            }

            bool hasCombinationalWrite = false;
            slang::SourceLocation writeLocation{};
            for (const auto& entry : writeBack_.entries)
            {
                if (!entry.target.valid() || entry.target.index != init.reg.index)
                {
                    continue;
                }
                if (entry.domain == ControlDomain::Sequential ||
                    entry.domain == ControlDomain::Latch)
                {
                    continue;
                }
                hasCombinationalWrite = true;
                writeLocation = entry.location;
                break;
            }
            if (hasCombinationalWrite)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        writeLocation.valid() ? writeLocation : init.location,
                        "Skipping register initializer on combinationally-driven target");
                }
                continue;
            }

            current = StorageKind::Register;
            if (init.location.valid())
            {
                storageLocation_[init.reg.index] = init.location;
            }
        }
    }

    void createStorageDeclarations()
    {
        for (std::size_t index = 0; index < storageBySymbol_.size(); ++index)
        {
            if (storageBySymbol_[index] == StorageKind::None)
            {
                continue;
            }
            PlanSymbolId symbol;
            symbol.index = static_cast<uint32_t>(index);
            std::string_view name = plan_.symbolTable.text(symbol);
            if (name.empty())
            {
                continue;
            }
            markDeclaredSymbol(symbol);
            const auto info = resolveStorageInfo(symbol);
            if (!info)
            {
                continue;
            }
            const bool isRegister = storageBySymbol_[index] == StorageKind::Register;
            const wolvrix::lib::grh::OperationKind kind =
                isRegister ? wolvrix::lib::grh::OperationKind::kRegister
                           : wolvrix::lib::grh::OperationKind::kLatch;
            wolvrix::lib::grh::SymbolId sym = graph_.lookupSymbol(name);
            if (!sym.valid())
            {
                sym = graph_.internSymbol(name);
            }
            if (!sym.valid())
            {
                throw std::runtime_error("Storage symbol already bound to value/operation: " +
                                         std::string(name));
            }
            slang::SourceLocation location = storageLocation_[index];
            wolvrix::lib::grh::OperationId op = createOp(kind, sym, location);
            graph_.setAttr(op, "width", static_cast<int64_t>(info->width));
            graph_.setAttr(op, "isSigned", info->isSigned);
            if (isRegister)
            {
                applyRegisterInit(op, symbol);
            }
            storageOpBySymbol_[index] = op;
            ensureStorageReadValue(symbol, storageBySymbol_[index], location);
        }
    }


    void createPortValues()
    {
        for (const auto& port : plan_.ports)
        {
            if (!port.symbol.valid())
            {
                continue;
            }
            markDeclaredSymbol(port.symbol);
            const std::string_view portName = plan_.symbolTable.text(port.symbol);
            if (portName.empty())
            {
                continue;
            }
            const int32_t width = normalizeWidth(port.width);
            if (port.direction == PortDirection::Input)
            {
                if (isStorageSymbol(port.symbol))
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->warn(
                            slang::SourceLocation{},
                            "Skipping input port bound to storage symbol");
                    }
                    continue;
                }
                wolvrix::lib::grh::ValueId value =
                    createValue(port.symbol, width, port.isSigned, port.valueType);
                if (value.valid())
                {
                    graph_.bindInputPort(portName, value);
                }
                continue;
            }
            if (port.direction == PortDirection::Output)
            {
                wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
                if (isStorageSymbol(port.symbol))
                {
                    value = valueForSymbol(port.symbol);
                    if (!value.valid())
                    {
                        value = ensureStorageReadValue(port.symbol,
                                                       storageBySymbol_[port.symbol.index],
                                                       slang::SourceLocation{});
                    }
                }
                else
                {
                    if (isMemoryBackedPackedAggregate(port.symbol))
                    {
                        value = emitPackedAggregateWholeRead(port.symbol, slang::SourceLocation{});
                    }
                    else
                    {
                        value = createValue(port.symbol, width, port.isSigned, port.valueType);
                    }
                }
                if (value.valid())
                {
                    graph_.bindOutputPort(portName, value);
                }
                continue;
            }
            if (port.direction == PortDirection::Inout && port.inoutSymbol)
            {
                const auto& binding = *port.inoutSymbol;
                if (port.valueType != wolvrix::lib::grh::ValueType::Logic && context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        slang::SourceLocation{},
                        "Inout ports for real/string are unsupported; treating as logic");
                }
                const wolvrix::lib::grh::ValueType inoutType = wolvrix::lib::grh::ValueType::Logic;
                wolvrix::lib::grh::ValueId inValue =
                    createValue(binding.inSymbol, width, port.isSigned, inoutType);
                wolvrix::lib::grh::ValueId outValue =
                    createValue(binding.outSymbol, width, port.isSigned, inoutType);
                wolvrix::lib::grh::ValueId oeValue =
                    createValue(binding.oeSymbol, width, false, wolvrix::lib::grh::ValueType::Logic);
                if (inValue.valid() && outValue.valid() && oeValue.valid())
                {
                    graph_.bindInoutPort(portName, inValue, outValue,
                                         oeValue);
                }
                continue;
            }
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(
                    slang::SourceLocation{},
                    "Skipping unsupported port direction in graph assembly");
            }
        }
    }

    void createSignalValues()
    {
        for (const auto& signal : plan_.signals)
        {
            if (!signal.symbol.valid())
            {
                continue;
            }
            markDeclaredSymbol(signal.symbol);
            if (isStorageSymbol(signal.symbol))
            {
                continue;
            }
            if (isMemoryBackedPackedAggregate(signal.symbol))
            {
                continue;
            }
            if (!isFlattenedNetArray(signal) && !isPackedAggregateVariable(signal) &&
                (signal.memoryRows > 0 || signal.kind == SignalKind::Memory))
            {
                continue;
            }
            if (signal.valueType != wolvrix::lib::grh::ValueType::Logic &&
                (signal.memoryRows > 0 || signal.kind == SignalKind::Memory))
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        slang::SourceLocation{},
                        "Ignoring memory-like real/string signal");
                }
                continue;
            }
            const int32_t width =
                normalizeWidth(static_cast<int32_t>(
                    isPackedAggregateVariable(signal) ? flattenedAggregateWidth(signal)
                                                      : flattenedNetWidth(signal)));
            createValue(signal.symbol, width, signal.isSigned, signal.valueType);
        }
    }

    // Emits the generate-scope copy groups collected by collectSignals. Group
    // members are the same SymbolIds that were marked declared, so the group
    // list is pure annotation on top of the declared set.
    void emitGenerateGroups()
    {
        auto lookupOrIntern = [&](std::string_view text) -> wolvrix::lib::grh::SymbolId
        {
            wolvrix::lib::grh::SymbolId sym = graph_.lookupSymbol(text);
            if (!sym.valid())
            {
                sym = graph_.internSymbol(text);
            }
            if (!sym.valid())
            {
                throw std::runtime_error(
                    "Generate group symbol is already bound to value/operation: " +
                    std::string(text));
            }
            return sym;
        };
        for (const auto& groupPlan : plan_.generateGroups)
        {
            const wolvrix::lib::grh::SymbolId scope = lookupOrIntern(groupPlan.scope);
            const wolvrix::lib::grh::SymbolId name = lookupOrIntern(groupPlan.name);
            const std::size_t groupIndex = graph_.addGenerateGroup(scope, name);
            for (const PlanSymbolId member : groupPlan.symbols)
            {
                const wolvrix::lib::grh::SymbolId sym = symbolForPlan(member);
                if (sym.valid())
                {
                    graph_.addGenerateGroupSymbol(groupIndex, sym);
                }
            }
        }
    }

    const SignalInfo* findSignalInfo(PlanSymbolId symbol) const
    {
        if (!symbol.valid())
        {
            return nullptr;
        }
        for (const auto& signal : plan_.signals)
        {
            if (signal.symbol.index == symbol.index)
            {
                return &signal;
            }
        }
        return nullptr;
    }

    SignalId findSignalId(PlanSymbolId symbol) const
    {
        if (!symbol.valid())
        {
            return kInvalidPlanIndex;
        }
        for (SignalId i = 0; i < static_cast<SignalId>(plan_.signals.size()); ++i)
        {
            if (plan_.signals[i].symbol.index == symbol.index)
            {
                return i;
            }
        }
        return kInvalidPlanIndex;
    }

    bool isMemoryBackedPackedAggregate(PlanSymbolId symbol) const
    {
        const SignalInfo* signal = findSignalInfo(symbol);
        if (!signal || !isPackedAggregateVariable(*signal))
        {
            return false;
        }
        for (const auto& read : lowering_.memoryReads)
        {
            if (read.memory.index == symbol.index)
            {
                return true;
            }
        }
        for (const auto& write : lowering_.memoryWrites)
        {
            if (write.memory.index == symbol.index)
            {
                return true;
            }
        }
        for (const auto& fill : lowering_.memoryFills)
        {
            if (fill.memory.index == symbol.index)
            {
                return true;
            }
        }
        return false;
    }

    wolvrix::lib::grh::ValueId emitPackedAggregateWholeRead(PlanSymbolId symbol,
                                                            slang::SourceLocation location)
    {
        if (!symbol.valid() || symbol.index >= packedAggregateWholeReadBySymbol_.size())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (packedAggregateWholeReadBySymbol_[symbol.index].valid())
        {
            return packedAggregateWholeReadBySymbol_[symbol.index];
        }
        const SignalId signalId = findSignalId(symbol);
        if (signalId == kInvalidPlanIndex)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        const SignalInfo& signal = plan_.signals[signalId];
        if (!isPackedAggregateVariable(signal) || signal.memoryRows <= 0)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (!ensureMemoryOp(symbol, signalId, location))
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        std::string_view memSymbol = memorySymbolText(symbol);
        if (memSymbol.empty())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }

        const int32_t rowWidth = normalizeWidth(signal.width);
        const int64_t rowCount = signal.memoryRows;
        std::vector<wolvrix::lib::grh::ValueId> rowValues;
        rowValues.reserve(static_cast<std::size_t>(rowCount));
        for (int64_t row = 0; row < rowCount; ++row)
        {
            ExprNode addressNode;
            addressNode.kind = ExprNodeKind::Constant;
            addressNode.literal = std::to_string(row);
            addressNode.widthHint = 32;
            addressNode.location = location;
            wolvrix::lib::grh::ValueId addressValue = emitConstant(addressNode);
            if (!addressValue.valid())
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }

            wolvrix::lib::grh::ValueId dataValue =
                graph_.createValue(makeInternalValueSymbol(), rowWidth,
                                   signal.isSigned, signal.valueType);
            wolvrix::lib::grh::OperationId readOp =
                createOp(wolvrix::lib::grh::OperationKind::kMemoryReadPort,
                         makeInternalOpSymbol(), location);
            graph_.addOperand(readOp, addressValue);
            graph_.setAttr(readOp, "memSymbol", std::string(memSymbol));
            graph_.addResult(readOp, dataValue);
            rowValues.push_back(dataValue);
        }
        if (rowValues.empty())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (rowValues.size() == 1)
        {
            packedAggregateWholeReadBySymbol_[symbol.index] = rowValues.front();
            return rowValues.front();
        }

        wolvrix::lib::grh::OperationId concatOp =
            createOp(wolvrix::lib::grh::OperationKind::kConcat,
                     wolvrix::lib::grh::SymbolId::invalid(),
                     location);
        for (auto it = rowValues.rbegin(); it != rowValues.rend(); ++it)
        {
            graph_.addOperand(concatOp, *it);
        }
        const int32_t aggregateWidth =
            normalizeWidth(static_cast<int32_t>(flattenedAggregateWidth(signal)));
        wolvrix::lib::grh::ValueId result =
            graph_.createValue(makeInternalValueSymbol(), aggregateWidth,
                               signal.isSigned, signal.valueType);
        graph_.addResult(concatOp, result);
        packedAggregateWholeReadBySymbol_[symbol.index] = result;
        return result;
    }

    const slang::ast::Type* lookupPlanSymbolType(PlanSymbolId symbol) const
    {
        if (!symbol.valid() || !plan_.body)
        {
            return nullptr;
        }
        const std::string_view name = plan_.symbolTable.text(symbol);
        if (name.empty())
        {
            return nullptr;
        }
        const slang::ast::Symbol* astSymbol = plan_.body->find(name);
        if (!astSymbol)
        {
            return nullptr;
        }
        if (const auto* value = astSymbol->as_if<slang::ast::ValueSymbol>())
        {
            return &value->getType();
        }
        return nullptr;
    }

    void annotatePackedArrayConcatDefop(PlanSymbolId target,
                                        wolvrix::lib::grh::ValueId value)
    {
        if (!target.valid() || !value.valid())
        {
            return;
        }
        const SignalInfo* signal = findSignalInfo(target);
        if (!signal || signal->packedDims.size() < 2)
        {
            return;
        }
        const slang::ast::Type* type = lookupPlanSymbolType(target);
        if (!type || !plan_.body)
        {
            return;
        }
        const auto& canonical = type->getCanonicalType();
        if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
        {
            return;
        }
        const auto& packed = canonical.as<slang::ast::PackedArrayType>();
        const uint64_t countRaw = packed.range.fullWidth();
        const uint64_t elementWidthRaw =
            computeFixedWidth(packed.elementType, *plan_.body, context_.diagnostics);
        if (countRaw == 0 || elementWidthRaw == 0 ||
            countRaw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            elementWidthRaw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return;
        }
        const int64_t elementCount = static_cast<int64_t>(countRaw);
        const int64_t elementWidth = static_cast<int64_t>(elementWidthRaw);
        if (elementCount <= 1 || elementWidth <= 0)
        {
            return;
        }

        const wolvrix::lib::grh::Value resultValue = graph_.getValue(value);
        const wolvrix::lib::grh::OperationId defOpId = resultValue.definingOp();
        if (!defOpId.valid())
        {
            return;
        }
        const wolvrix::lib::grh::Operation defOp = graph_.getOperation(defOpId);
        if (defOp.kind() != wolvrix::lib::grh::OperationKind::kConcat ||
            defOp.results().size() != 1 ||
            static_cast<int64_t>(defOp.operands().size()) != elementCount ||
            resultValue.width() != elementCount * elementWidth)
        {
            return;
        }
        for (const auto operand : defOp.operands())
        {
            if (!operand.valid() || graph_.getValue(operand).width() != elementWidth)
            {
                return;
            }
        }

        const int64_t indexLow = std::min<int64_t>(packed.range.left, packed.range.right);
        const int64_t indexHigh = std::max<int64_t>(packed.range.left, packed.range.right);
        const bool indexDownto = packed.range.left > packed.range.right;
        graph_.setAttr(defOpId, "svPackedArray.version", static_cast<int64_t>(1));
        graph_.setAttr(defOpId, "svPackedArray.elementWidth", elementWidth);
        graph_.setAttr(defOpId, "svPackedArray.elementCount", elementCount);
        graph_.setAttr(defOpId, "svPackedArray.indexLow", indexLow);
        graph_.setAttr(defOpId, "svPackedArray.indexHigh", indexHigh);
        graph_.setAttr(defOpId, "svPackedArray.indexDirection",
                       std::string(indexDownto ? "downto" : "upto"));
        graph_.setAttr(defOpId, "svPackedArray.laneOrder",
                       std::string(indexDownto ? "lsb_index_low" : "lsb_index_high"));
        graph_.setAttr(defOpId, "svPackedArray.concat.operand0Index",
                       indexDownto ? indexHigh : indexLow);
        graph_.setAttr(defOpId, "svPackedArray.concat.operandStride",
                       static_cast<int64_t>(indexDownto ? -1 : 1));
    }

    const InoutSignalInfo* findInoutSignalInfo(PlanSymbolId symbol) const
    {
        if (!symbol.valid())
        {
            return nullptr;
        }
        for (const auto& info : plan_.inoutSignals)
        {
            if (info.symbol.index == symbol.index ||
                info.binding.inSymbol.index == symbol.index ||
                info.binding.outSymbol.index == symbol.index ||
                info.binding.oeSymbol.index == symbol.index)
            {
                return &info;
            }
        }
        return nullptr;
    }

    void createInoutSignalValues()
    {
        for (const auto& info : plan_.inoutSignals)
        {
            const SignalInfo* signal = findSignalInfo(info.symbol);
            if (!signal)
            {
                continue;
            }
            const int32_t width =
                normalizeWidth(static_cast<int32_t>(flattenedNetWidth(*signal)));
            if (signal->valueType != wolvrix::lib::grh::ValueType::Logic && context_.diagnostics)
            {
                context_.diagnostics->warn(
                    slang::SourceLocation{},
                    "Inout signals for real/string are unsupported; treating as logic");
            }
            createValue(info.binding.outSymbol, width, signal->isSigned,
                        wolvrix::lib::grh::ValueType::Logic);
            createValue(info.binding.oeSymbol, width, false, wolvrix::lib::grh::ValueType::Logic);
        }
    }

    const SignalInfo* memorySignal(SignalId signal) const
    {
        if (signal == kInvalidPlanIndex ||
            signal >= static_cast<SignalId>(plan_.signals.size()))
        {
            return nullptr;
        }
        return &plan_.signals[signal];
    }

    void applyMemoryInit(wolvrix::lib::grh::OperationId op, PlanSymbolId memory)
    {
        if (!memory.valid())
        {
            return;
        }
        std::vector<std::string> kinds;
        std::vector<std::string> files;
        std::vector<std::string> initValues;
        std::vector<int64_t> starts;
        std::vector<int64_t> lens;
        for (const auto& init : lowering_.memoryInits)
        {
            if (init.memory.index != memory.index)
            {
                continue;
            }
            kinds.push_back(init.kind);
            files.push_back(init.file);
            initValues.push_back(init.initValue);
            starts.push_back(init.start);
            lens.push_back(init.len);
        }
        if (kinds.empty())
        {
            return;
        }
        graph_.setAttr(op, "initKind", std::move(kinds));
        graph_.setAttr(op, "initFile", std::move(files));
        graph_.setAttr(op, "initValue", std::move(initValues));
        graph_.setAttr(op, "initStart", std::move(starts));
        graph_.setAttr(op, "initLen", std::move(lens));
    }

    void applyRegisterInit(wolvrix::lib::grh::OperationId op, PlanSymbolId reg)
    {
        if (!reg.valid())
        {
            return;
        }
        const RegisterInit* finalInit = nullptr;
        for (const auto& init : lowering_.registerInits)
        {
            if (init.reg.index != reg.index)
            {
                continue;
            }
            finalInit = &init;
        }
        if (!finalInit)
        {
            return;
        }
        graph_.setAttr(op, "initValue", finalInit->initValue);
    }

    bool ensureMemoryOp(PlanSymbolId memory, SignalId signal, slang::SourceLocation location)
    {
        if (!memory.valid() || memory.index >= memoryOpBySymbol_.size())
        {
            return false;
        }
        if (memoryOpBySymbol_[memory.index].valid())
        {
            return true;
        }
        const SignalInfo* info = memorySignal(signal);
        if (!info)
        {
            return false;
        }
        if (info->memoryRows <= 0)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(location,
                                           "Skipping memory op without valid row count");
            }
            return false;
        }

        std::string name(plan_.symbolTable.text(memory));
        if (name.empty())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(location,
                                           "Skipping memory op without symbol name");
            }
            return false;
        }
        std::string finalName = name;
        wolvrix::lib::grh::SymbolId sym = graph_.lookupSymbol(finalName);
        if (sym.valid())
        {
            if (graph_.findValue(sym).valid() || graph_.findOperation(sym).valid())
            {
                sym = makeInternalOpSymbol();
                finalName = std::string(graph_.symbolText(sym));
            }
        }
        else
        {
            sym = graph_.internSymbol(finalName);
            if (!sym.valid())
            {
                sym = makeInternalOpSymbol();
                finalName = std::string(graph_.symbolText(sym));
            }
        }

        const int64_t width = normalizeWidth(info->width);
        if (info->width <= 0 && context_.diagnostics)
        {
            context_.diagnostics->warn(location, "Memory width missing, defaulting to 1");
        }

        wolvrix::lib::grh::OperationId op =
            createOp(wolvrix::lib::grh::OperationKind::kMemory, sym, location);
        graph_.setAttr(op, "width", width);
        graph_.setAttr(op, "row", static_cast<int64_t>(info->memoryRows));
        graph_.setAttr(op, "isSigned", info->isSigned);
        applyMemoryInit(op, memory);
        memoryOpBySymbol_[memory.index] = op;
        memorySymbolName_[memory.index] = finalName;
        return true;
    }

    std::string_view memorySymbolText(PlanSymbolId memory) const
    {
        if (!memory.valid() || memory.index >= memorySymbolName_.size())
        {
            return {};
        }
        return memorySymbolName_[memory.index];
    }

    void createMemoryOps()
    {
        for (SignalId i = 0; i < static_cast<SignalId>(plan_.signals.size()); ++i)
        {
            const auto& signal = plan_.signals[i];
            if (!signal.symbol.valid())
            {
                continue;
            }
            if (signal.kind == SignalKind::Net)
            {
                continue;
            }
            if (isPackedAggregateVariable(signal))
            {
                bool usedAsMemory = false;
                for (const auto& read : lowering_.memoryReads)
                {
                    if (read.memory.valid() &&
                        read.memory.index == signal.symbol.index)
                    {
                        usedAsMemory = true;
                        break;
                    }
                }
                if (!usedAsMemory)
                {
                    for (const auto& write : lowering_.memoryWrites)
                    {
                        if (write.memory.valid() &&
                            write.memory.index == signal.symbol.index)
                        {
                            usedAsMemory = true;
                            break;
                        }
                    }
                }
                if (!usedAsMemory)
                {
                    for (const auto& fill : lowering_.memoryFills)
                    {
                        if (fill.memory.valid() &&
                            fill.memory.index == signal.symbol.index)
                        {
                            usedAsMemory = true;
                            break;
                        }
                    }
                }
                if (!usedAsMemory)
                {
                    continue;
                }
            }
            if (signal.memoryRows <= 0 && signal.kind != SignalKind::Memory)
            {
                continue;
            }
            ensureMemoryOp(signal.symbol, i, slang::SourceLocation{});
        }
    }

    void emitMemoryPorts()
    {
        for (const auto& entry : lowering_.memoryReads)
        {
            ExprNodeId id = entry.replacement;
            if (id == kInvalidPlanIndex)
            {
                id = entry.data;
            }
            if (id == kInvalidPlanIndex)
            {
                continue;
            }
            emitMemoryRead(id);
        }
        emitMemoryFills();
        emitMemoryWrites();
    }

    wolvrix::lib::grh::ValueId emitMemoryRead(ExprNodeId id)
    {
        if (id == kInvalidPlanIndex || id >= memoryReadIndexByExpr_.size())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (valueByExpr_[id].valid())
        {
            return valueByExpr_[id];
        }
        const int32_t readIndex = memoryReadIndexByExpr_[id];
        if (readIndex == kInvalidMemoryReadIndex)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (readIndex < 0 ||
            static_cast<std::size_t>(readIndex) >= lowering_.memoryReads.size())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        const MemoryReadPort& entry = lowering_.memoryReads[readIndex];
        if (entry.isSync && entry.eventEdges.empty())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(
                    entry.location,
                    "Skipping synchronous memory read without edge-sensitive timing control");
            }
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (!ensureMemoryOp(entry.memory, entry.signal, entry.location))
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        const SignalInfo* info = memorySignal(entry.signal);
        if (!info)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        std::string_view memSymbol = memorySymbolText(entry.memory);
        if (memSymbol.empty())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }

        wolvrix::lib::grh::ValueId dataValue = findReusableMemoryRead(entry);
        if (!dataValue.valid())
        {
            const int32_t width = normalizeWidth(info->width);
            wolvrix::lib::grh::SymbolId dataSym = makeInternalValueSymbol();
            dataValue =
                graph_.createValue(dataSym, width, info->isSigned, info->valueType);

            wolvrix::lib::grh::ValueId addressValue = emitExpr(entry.address);
            if (!addressValue.valid())
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }

            wolvrix::lib::grh::SymbolId readSym = makeInternalOpSymbol();
            wolvrix::lib::grh::OperationId readOp =
                createOp(wolvrix::lib::grh::OperationKind::kMemoryReadPort, readSym, entry.location);
            graph_.addOperand(readOp, addressValue);
            graph_.setAttr(readOp, "memSymbol", std::string(memSymbol));

            graph_.addResult(readOp, dataValue);
            emittedMemoryReads_.push_back(EmittedMemoryRead{
                .memory = entry.memory,
                .signal = entry.signal,
                .address = entry.address,
                .isSync = entry.isSync,
                .updateCond = entry.updateCond,
                .eventEdges = entry.eventEdges,
                .eventOperands = entry.eventOperands,
                .expr = id,
                .value = dataValue,
            });
        }
        valueByExpr_[id] = dataValue;
        return dataValue;
    }

    wolvrix::lib::grh::ValueId findReusableMemoryRead(const MemoryReadPort& entry) const
    {
        for (const auto& emitted : emittedMemoryReads_)
        {
            if (emitted.memory.index != entry.memory.index ||
                emitted.signal != entry.signal ||
                emitted.isSync != entry.isSync ||
                emitted.eventEdges != entry.eventEdges)
            {
                continue;
            }
            if (!exprEquivalent(emitted.address, entry.address) ||
                !exprEquivalent(emitted.updateCond, entry.updateCond) ||
                !exprListEquivalent(emitted.eventOperands, entry.eventOperands))
            {
                continue;
            }
            return emitted.value;
        }
        return wolvrix::lib::grh::ValueId::invalid();
    }

    bool exprListEquivalent(const std::vector<ExprNodeId>& lhs,
                            const std::vector<ExprNodeId>& rhs) const
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i)
        {
            if (!exprEquivalent(lhs[i], rhs[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool exprEquivalent(ExprNodeId lhs, ExprNodeId rhs) const
    {
        std::unordered_set<uint64_t> seen;
        return exprEquivalentImpl(lhs, rhs, seen);
    }

    bool exprEquivalentImpl(ExprNodeId lhs, ExprNodeId rhs,
                            std::unordered_set<uint64_t>& seen) const
    {
        if (lhs == rhs)
        {
            return true;
        }
        if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex ||
            lhs >= lowering_.values.size() || rhs >= lowering_.values.size())
        {
            return false;
        }
        const uint64_t key = (static_cast<uint64_t>(lhs) << 32) |
                             static_cast<uint32_t>(rhs);
        if (!seen.insert(key).second)
        {
            return true;
        }

        const ExprNode& lhsNode = lowering_.values[lhs];
        const ExprNode& rhsNode = lowering_.values[rhs];
        if (lhsNode.kind != rhsNode.kind ||
            lhsNode.widthHint != rhsNode.widthHint ||
            lhsNode.isSigned != rhsNode.isSigned ||
            lhsNode.valueType != rhsNode.valueType)
        {
            return false;
        }
        switch (lhsNode.kind)
        {
        case ExprNodeKind::Constant:
            return lhsNode.literal == rhsNode.literal;
        case ExprNodeKind::Symbol:
            return lhsNode.symbol.index == rhsNode.symbol.index;
        case ExprNodeKind::Operation:
            if (lhsNode.op != rhsNode.op ||
                lhsNode.operands.size() != rhsNode.operands.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < lhsNode.operands.size(); ++i)
            {
                if (!exprEquivalentImpl(lhsNode.operands[i], rhsNode.operands[i], seen))
                {
                    return false;
                }
            }
            return true;
        case ExprNodeKind::XmrRead:
            return lhsNode.xmrPath == rhsNode.xmrPath;
        default:
            return false;
        }
    }

    void emitMemoryWrites()
    {
        for (const auto& entry : lowering_.memoryWrites)
        {
            if (!entry.memory.valid())
            {
                continue;
            }
            if (entry.eventEdges.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        entry.location,
                        "Skipping memory write without edge-sensitive timing control");
                }
                continue;
            }
            if (!ensureMemoryOp(entry.memory, entry.signal, entry.location))
            {
                continue;
            }
            std::string_view memSymbol = memorySymbolText(entry.memory);
            if (memSymbol.empty())
            {
                continue;
            }
            wolvrix::lib::grh::ValueId updateCond = emitExpr(entry.updateCond);
            wolvrix::lib::grh::ValueId address = emitExpr(entry.address);
            wolvrix::lib::grh::ValueId data = emitExpr(entry.data);
            wolvrix::lib::grh::ValueId mask = emitExpr(entry.mask);
            if (!updateCond.valid() || !address.valid() || !data.valid() || !mask.valid())
            {
                continue;
            }

            wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol();
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kMemoryWritePort, opSym, entry.location);
            graph_.addOperand(op, updateCond);
            graph_.addOperand(op, address);
            graph_.addOperand(op, data);
            graph_.addOperand(op, mask);
            for (ExprNodeId evtId : entry.eventOperands)
            {
                wolvrix::lib::grh::ValueId evt = emitExpr(evtId);
                if (!evt.valid())
                {
                    continue;
                }
                graph_.addOperand(op, evt);
            }
            std::vector<std::string> edges;
            edges.reserve(entry.eventEdges.size());
            for (EventEdge edge : entry.eventEdges)
            {
                edges.push_back(edgeText(edge));
            }
            graph_.setAttr(op, "eventEdge", std::move(edges));
            graph_.setAttr(op, "memSymbol", std::string(memSymbol));
        }
    }

    void emitMemoryFills()
    {
        for (const auto& entry : lowering_.memoryFills)
        {
            if (!entry.memory.valid())
            {
                continue;
            }
            if (entry.eventEdges.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        entry.location,
                        "Skipping memory fill without edge-sensitive timing control");
                }
                continue;
            }
            if (!ensureMemoryOp(entry.memory, entry.signal, entry.location))
            {
                continue;
            }
            std::string_view memSymbol = memorySymbolText(entry.memory);
            if (memSymbol.empty())
            {
                continue;
            }
            wolvrix::lib::grh::ValueId updateCond = emitExpr(entry.updateCond);
            wolvrix::lib::grh::ValueId data = emitExpr(entry.data);
            if (!updateCond.valid() || !data.valid())
            {
                continue;
            }

            wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol();
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kMemoryFillPort, opSym, entry.location);
            graph_.addOperand(op, updateCond);
            graph_.addOperand(op, data);
            for (ExprNodeId evtId : entry.eventOperands)
            {
                wolvrix::lib::grh::ValueId evt = emitExpr(evtId);
                if (!evt.valid())
                {
                    continue;
                }
                graph_.addOperand(op, evt);
            }
            std::vector<std::string> edges;
            edges.reserve(entry.eventEdges.size());
            for (EventEdge edge : entry.eventEdges)
            {
                edges.push_back(edgeText(edge));
            }
            graph_.setAttr(op, "eventEdge", std::move(edges));
            graph_.setAttr(op, "memSymbol", std::string(memSymbol));
        }
    }

    void emitSideEffects()
    {
        emitDpiImports();
        for (const auto& stmt : lowering_.loweredStmts)
        {
            switch (stmt.kind)
            {
            case LoweredStmtKind::SystemTask:
                emitSystemTask(stmt);
                break;
            case LoweredStmtKind::DpiCall:
                emitDpiCall(stmt);
                break;
            default:
                break;
            }
        }
    }

    const DpiImportInfo* findDpiImport(std::string_view symbol) const
    {
        for (const auto& info : lowering_.dpiImports)
        {
            if (info.symbol == symbol)
            {
                return &info;
            }
        }
        return nullptr;
    }

    void emitDpiImports()
    {
        for (const auto& info : lowering_.dpiImports)
        {
            if (info.symbol.empty())
            {
                continue;
            }
            wolvrix::lib::grh::SymbolId sym = graph_.lookupSymbol(info.symbol);
            if (sym.valid())
            {
                wolvrix::lib::grh::OperationId existing = graph_.findOperation(sym);
                if (existing.valid())
                {
                    if (graph_.getOperation(existing).kind() != wolvrix::lib::grh::OperationKind::kDpicImport &&
                        context_.diagnostics)
                    {
                        context_.diagnostics->warn(
                            slang::SourceLocation{},
                            std::string("Skipping DPI import with conflicting symbol ") +
                                info.symbol);
                    }
                    continue;
                }
                if (graph_.findValue(sym).valid())
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->warn(
                            slang::SourceLocation{},
                            std::string("Skipping DPI import with conflicting value ") +
                                info.symbol);
                    }
                    continue;
                }
            }
            else
            {
                sym = graph_.internSymbol(info.symbol);
                if (!sym.valid())
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->warn(
                            slang::SourceLocation{},
                            std::string("Skipping DPI import with conflicting symbol ") +
                                info.symbol);
                    }
                    continue;
                }
            }

            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kDpicImport, sym, slang::SourceLocation{});
            graph_.setAttr(op, "argsDirection", info.argsDirection);
            graph_.setAttr(op, "argsWidth", info.argsWidth);
            graph_.setAttr(op, "argsName", info.argsName);
            graph_.setAttr(op, "argsSigned", info.argsSigned);
            graph_.setAttr(op, "argsType", info.argsType);
            graph_.setAttr(op, "hasReturn", info.hasReturn);
            if (info.hasReturn)
            {
                graph_.setAttr(op, "returnWidth", info.returnWidth);
                graph_.setAttr(op, "returnSigned", info.returnSigned);
                if (!info.returnType.empty())
                {
                    graph_.setAttr(op, "returnType", info.returnType);
                }
            }
        }
    }

    static std::string procKindText(ProcKind kind)
    {
        switch (kind)
        {
        case ProcKind::Initial:
            return "initial";
        case ProcKind::Final:
            return "final";
        case ProcKind::AlwaysComb:
            return "always_comb";
        case ProcKind::AlwaysLatch:
            return "always_latch";
        case ProcKind::AlwaysFF:
            return "always_ff";
        case ProcKind::Always:
            return "always";
        default:
            return "unknown";
        }
    }

    void emitSystemTask(const LoweredStmt& stmt)
    {
        if (stmt.eventEdges.size() != stmt.eventOperands.size())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(stmt.location,
                                           "Skipping system task with mismatched event binding");
            }
            return;
        }

        wolvrix::lib::grh::ValueId updateCond = emitExpr(stmt.updateCond);
        if (!updateCond.valid())
        {
            return;
        }

        wolvrix::lib::grh::OperationId op =
            createOp(wolvrix::lib::grh::OperationKind::kSystemTask,
                     wolvrix::lib::grh::SymbolId::invalid(),
                     stmt.location);
        graph_.addOperand(op, updateCond);

        for (ExprNodeId argId : stmt.systemTask.args)
        {
            wolvrix::lib::grh::ValueId arg = emitExpr(argId);
            if (!arg.valid())
            {
                return;
            }
            graph_.addOperand(op, arg);
        }
        for (ExprNodeId evtId : stmt.eventOperands)
        {
            wolvrix::lib::grh::ValueId evt = emitExpr(evtId);
            if (!evt.valid())
            {
                return;
            }
            graph_.addOperand(op, evt);
        }

        if (stmt.systemTask.name.empty())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(stmt.location,
                                           "Skipping system task without name");
            }
            return;
        }
        graph_.setAttr(op, "name", stmt.systemTask.name);
        std::vector<std::string> edges;
        edges.reserve(stmt.eventEdges.size());
        for (EventEdge edge : stmt.eventEdges)
        {
            edges.push_back(edgeText(edge));
        }
        if (!edges.empty())
        {
            graph_.setAttr(op, "eventEdge", edges);
            if (edges.size() == 1)
            {
                graph_.setAttr(op, "clkPolarity", edges.front());
            }
        }
        graph_.setAttr(op, "procKind", procKindText(stmt.procKind));
        graph_.setAttr(op, "hasTiming", stmt.hasTiming);
    }

    std::optional<std::pair<int64_t, bool>> findDpiArgType(
        const DpiImportInfo& importInfo, std::string_view name,
        std::string_view direction) const
    {
        for (std::size_t i = 0; i < importInfo.argsName.size(); ++i)
        {
            if (importInfo.argsName[i] != name)
            {
                continue;
            }
            if (i >= importInfo.argsDirection.size() || i >= importInfo.argsWidth.size() ||
                i >= importInfo.argsSigned.size())
            {
                break;
            }
            if (!direction.empty() && importInfo.argsDirection[i] != direction)
            {
                break;
            }
            return std::pair<int64_t, bool>{importInfo.argsWidth[i], importInfo.argsSigned[i]};
        }
        return std::nullopt;
    }

    static wolvrix::lib::grh::ValueType dpiValueType(std::string_view typeName)
    {
        if (typeName.empty())
        {
            return wolvrix::lib::grh::ValueType::Logic;
        }
        std::string lowered;
        lowered.reserve(typeName.size());
        for (unsigned char ch : typeName)
        {
            lowered.push_back(static_cast<char>(std::tolower(ch)));
        }
        if (lowered == "real" || lowered == "shortreal")
        {
            return wolvrix::lib::grh::ValueType::Real;
        }
        if (lowered == "string")
        {
            return wolvrix::lib::grh::ValueType::String;
        }
        return wolvrix::lib::grh::ValueType::Logic;
    }

    wolvrix::lib::grh::ValueType findDpiArgValueType(const DpiImportInfo& importInfo,
                                           std::string_view name,
                                           std::string_view direction) const
    {
        for (std::size_t i = 0; i < importInfo.argsName.size(); ++i)
        {
            if (importInfo.argsName[i] != name)
            {
                continue;
            }
            if (!direction.empty() && i < importInfo.argsDirection.size() &&
                importInfo.argsDirection[i] != direction)
            {
                break;
            }
            if (i < importInfo.argsType.size())
            {
                return dpiValueType(importInfo.argsType[i]);
            }
            break;
        }
        return wolvrix::lib::grh::ValueType::Logic;
    }

    void emitDpiCall(const LoweredStmt& stmt)
    {
        if (stmt.eventEdges.size() != stmt.eventOperands.size())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(stmt.location,
                                           "Skipping DPI call with mismatched event binding");
            }
            return;
        }
        if (stmt.eventEdges.empty())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(stmt.location,
                                           "Skipping DPI call without edge-sensitive timing");
            }
            return;
        }

        const DpiCallStmt& dpi = stmt.dpiCall;
        const DpiImportInfo* importInfo = findDpiImport(dpi.targetImportSymbol);
        if (!importInfo)
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(
                    stmt.location,
                    std::string("Skipping DPI call without matching import ") +
                        dpi.targetImportSymbol);
            }
            return;
        }

        wolvrix::lib::grh::ValueId updateCond = emitExpr(stmt.updateCond);
        if (!updateCond.valid())
        {
            return;
        }

        wolvrix::lib::grh::OperationId op =
            createOp(wolvrix::lib::grh::OperationKind::kDpicCall,
                     wolvrix::lib::grh::SymbolId::invalid(),
                     stmt.location);
        graph_.addOperand(op, updateCond);
        for (ExprNodeId argId : dpi.inArgs)
        {
            wolvrix::lib::grh::ValueId arg = emitExpr(argId);
            if (!arg.valid())
            {
                return;
            }
            graph_.addOperand(op, arg);
        }
        for (ExprNodeId evtId : stmt.eventOperands)
        {
            wolvrix::lib::grh::ValueId evt = emitExpr(evtId);
            if (!evt.valid())
            {
                return;
            }
            graph_.addOperand(op, evt);
        }

        graph_.setAttr(op, "targetImportSymbol", dpi.targetImportSymbol);
        graph_.setAttr(op, "inArgName", dpi.inArgNames);
        graph_.setAttr(op, "outArgName", dpi.outArgNames);
        graph_.setAttr(op, "hasReturn", dpi.hasReturn);
        std::vector<std::string> edges;
        edges.reserve(stmt.eventEdges.size());
        for (EventEdge edge : stmt.eventEdges)
        {
            edges.push_back(edgeText(edge));
        }
        graph_.setAttr(op, "eventEdge", edges);
        if (edges.size() == 1)
        {
            graph_.setAttr(op, "clkPolarity", edges.front());
        }

        std::size_t resultOffset = 0;
        if (dpi.hasReturn)
        {
            if (dpi.results.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(stmt.location,
                                               "DPI call missing return result");
                }
                return;
            }
            PlanSymbolId retSymbol = dpi.results.front();
            const int64_t width = importInfo->returnWidth > 0 ? importInfo->returnWidth : 1;
            const bool isSigned = importInfo->returnSigned;
            wolvrix::lib::grh::ValueId retValue = valueForSymbol(retSymbol);
            if (!retValue.valid())
            {
                const wolvrix::lib::grh::ValueType retType =
                    dpiValueType(importInfo->returnType);
                retValue = createValue(retSymbol, static_cast<int32_t>(width), isSigned,
                                       retType);
            }
            if (!retValue.valid())
            {
                return;
            }
            graph_.addResult(op, retValue);
            resultOffset = 1;
        }

        if (dpi.results.size() < resultOffset + dpi.outArgNames.size())
        {
            if (context_.diagnostics)
            {
                context_.diagnostics->warn(stmt.location,
                                           "DPI call result count mismatch");
            }
            return;
        }

        for (std::size_t i = 0; i < dpi.outArgNames.size(); ++i)
        {
            PlanSymbolId resultSymbol = dpi.results[resultOffset + i];
            wolvrix::lib::grh::ValueId resultValue = valueForSymbol(resultSymbol);
            if (!resultValue.valid())
            {
                auto meta = findDpiArgType(*importInfo, dpi.outArgNames[i], "output");
                int64_t width = meta ? meta->first : 1;
                bool isSigned = meta ? meta->second : false;
                const wolvrix::lib::grh::ValueType valueType =
                    findDpiArgValueType(*importInfo, dpi.outArgNames[i], "output");
                resultValue =
                    createValue(resultSymbol, static_cast<int32_t>(width), isSigned, valueType);
            }
            if (!resultValue.valid())
            {
                return;
            }
            graph_.addResult(op, resultValue);
        }
    }

    void emitInstances()
    {
        struct OutputBinding {
            PlanSymbolId target;
            int64_t low = 0;
            int64_t width = 0;
            slang::SourceLocation location{};
            bool isXmr = false;
            bool isSlice = false;
            std::string xmrPath;
        };

        struct NetArrayBinding {
            PlanSymbolId target;
            int64_t low = 0;
            int64_t width = 0;
        };

        auto resolveNetArrayBinding = [&](const slang::ast::Expression& expr,
                                          NetArrayBinding& out) -> bool {
            const slang::ast::Expression* current = &expr;
            std::vector<const slang::ast::Expression*> selectors;
            while (const auto* select = current->as_if<slang::ast::ElementSelectExpression>())
            {
                const slang::ast::Type* valueType = select->value().type;
                if (!valueType || !valueType->isUnpackedArray())
                {
                    break;
                }
                selectors.push_back(&select->selector());
                current = &select->value();
            }
            if (selectors.empty())
            {
                return false;
            }
            const auto* baseNamed = current->as_if<slang::ast::NamedValueExpression>();
            if (!baseNamed || baseNamed->symbol.name.empty())
            {
                return false;
            }
            const PlanSymbolId baseId =
                lookupPlanSignal(plan_, baseNamed->symbol);
            const SignalInfo* signal = findSignalBySymbol(plan_, baseId);
            if (!signal || !isFlattenedNetArray(*signal))
            {
                return false;
            }
            if (selectors.size() > signal->unpackedDims.size())
            {
                return false;
            }
            std::reverse(selectors.begin(), selectors.end());

            const int64_t elementWidth = signal->width > 0 ? signal->width : 1;
            int64_t subarrayRows = 1;
            for (std::size_t i = selectors.size(); i < signal->unpackedDims.size(); ++i)
            {
                const int64_t extent =
                    signal->unpackedDims[i].extent > 0 ? signal->unpackedDims[i].extent : 1;
                if (subarrayRows > std::numeric_limits<int64_t>::max() / extent)
                {
                    subarrayRows = std::numeric_limits<int64_t>::max();
                    break;
                }
                subarrayRows *= extent;
            }
            int64_t sliceWidth = elementWidth;
            if (subarrayRows > 1)
            {
                if (elementWidth > std::numeric_limits<int64_t>::max() / subarrayRows)
                {
                    sliceWidth = std::numeric_limits<int64_t>::max();
                }
                else
                {
                    sliceWidth = elementWidth * subarrayRows;
                }
            }
            if (sliceWidth <= 0)
            {
                return false;
            }

            std::vector<int64_t> strides(signal->unpackedDims.size(), 1);
            int64_t stride = 1;
            for (std::size_t i = signal->unpackedDims.size(); i-- > 0;)
            {
                strides[i] = stride;
                const int64_t extent =
                    signal->unpackedDims[i].extent > 0 ? signal->unpackedDims[i].extent : 1;
                if (stride > std::numeric_limits<int64_t>::max() / extent)
                {
                    stride = std::numeric_limits<int64_t>::max();
                }
                else
                {
                    stride *= extent;
                }
            }

            int64_t address = 0;
            bool haveAddress = false;
            for (std::size_t i = 0; i < selectors.size(); ++i)
            {
                ExprNodeId index = connectionLowerer_.lowerExpression(*selectors[i]);
                auto indexConst = evalConstInt(plan_, lowering_, index);
                if (!indexConst)
                {
                    return false;
                }
                const auto& dim = signal->unpackedDims[i];
                int64_t normalized = 0;
                if (dim.left < dim.right)
                {
                    normalized = *indexConst - dim.left;
                }
                else
                {
                    normalized = dim.left - *indexConst;
                }
                const int64_t strideVal = strides[i];
                if (strideVal > 1)
                {
                    if (normalized > 0 &&
                        normalized > std::numeric_limits<int64_t>::max() / strideVal)
                    {
                        return false;
                    }
                    normalized *= strideVal;
                }
                if (!haveAddress)
                {
                    address = normalized;
                    haveAddress = true;
                }
                else
                {
                    if (normalized > 0 &&
                        address > std::numeric_limits<int64_t>::max() - normalized)
                    {
                        return false;
                    }
                    address += normalized;
                }
            }
            if (!haveAddress)
            {
                return false;
            }
            int64_t bitIndex = address;
            if (elementWidth > 1)
            {
                if (bitIndex > 0 &&
                    bitIndex > std::numeric_limits<int64_t>::max() / elementWidth)
                {
                    return false;
                }
                bitIndex *= elementWidth;
            }

            out.target = baseId;
            out.low = bitIndex;
            out.width = sliceWidth;
            return true;
        };

        auto resolveOutputBinding = [&](auto&& self,
                                        const slang::ast::Expression& expr,
                                        int64_t portWidth,
                                        OutputBinding& out) -> bool {
            if (const auto* assign = expr.as_if<slang::ast::AssignmentExpression>())
            {
                if (assign->isLValueArg())
                {
                    return self(self, assign->left(), portWidth, out);
                }
                return self(self, assign->right(), portWidth, out);
            }
            if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
            {
                if (conversion->isImplicit())
                {
                    return self(self, conversion->operand(), portWidth, out);
                }
                return false;
            }
            if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
            {
                out.target = lookupPlanSignal(plan_, named->symbol);
                out.low = 0;
                out.width = portWidth;
                out.location = expr.sourceRange.start();
                out.isSlice = false;
                return out.target.valid();
            }
            if (const auto* hier = expr.as_if<slang::ast::HierarchicalValueExpression>())
            {
                if (auto path = extractXmrPath(*hier))
                {
                    out.isXmr = true;
                    out.xmrPath = std::move(*path);
                    out.low = 0;
                    out.width = portWidth;
                    out.location = expr.sourceRange.start();
                    out.isSlice = false;
                    return true;
                }
                return false;
            }
            if (const auto* select = expr.as_if<slang::ast::ElementSelectExpression>())
            {
                NetArrayBinding netBinding;
                if (resolveNetArrayBinding(expr, netBinding))
                {
                    out.target = netBinding.target;
                    out.low = netBinding.low;
                    out.width = netBinding.width;
                    out.location = select->sourceRange.start();
                    out.isSlice = true;
                    return out.target.valid();
                }
                OutputBinding base;
                if (!self(self, select->value(), portWidth, base))
                {
                    return false;
                }
                ExprNodeId index = connectionLowerer_.lowerExpression(select->selector());
                auto indexConst = evalConstInt(plan_, lowering_, index);
                if (!indexConst)
                {
                    return false;
                }
                out.target = base.target;
                int64_t low = base.low;
                if (*indexConst > 0 &&
                    low > std::numeric_limits<int64_t>::max() - *indexConst)
                {
                    return false;
                }
                out.low = low + *indexConst;
                out.width = 1;
                out.location = select->sourceRange.start();
                out.isSlice = true;
                return out.target.valid();
            }
            if (const auto* range = expr.as_if<slang::ast::RangeSelectExpression>())
            {
                NetArrayBinding netBinding;
                if (resolveNetArrayBinding(range->value(), netBinding))
                {
                    ExprNodeId left = connectionLowerer_.lowerExpression(range->left());
                    ExprNodeId right = connectionLowerer_.lowerExpression(range->right());
                    auto leftConst = evalConstInt(plan_, lowering_, left);
                    auto rightConst = evalConstInt(plan_, lowering_, right);
                    if (!leftConst || !rightConst)
                    {
                        return false;
                    }
                    out.target = netBinding.target;
                    out.location = range->sourceRange.start();
                    out.isSlice = true;
                    switch (range->getSelectionKind())
                    {
                    case slang::ast::RangeSelectionKind::Simple: {
                        const int64_t lo = std::min(*leftConst, *rightConst);
                        const int64_t hi = std::max(*leftConst, *rightConst);
                        out.low = netBinding.low + lo;
                        out.width = hi - lo + 1;
                        break;
                    }
                    case slang::ast::RangeSelectionKind::IndexedUp:
                    case slang::ast::RangeSelectionKind::IndexedDown: {
                        if (*rightConst <= 0)
                        {
                            return false;
                        }
                        const int64_t width = *rightConst;
                        if (range->getSelectionKind() ==
                            slang::ast::RangeSelectionKind::IndexedDown)
                        {
                            out.low = netBinding.low + *leftConst - width + 1;
                        }
                        else
                        {
                            out.low = netBinding.low + *leftConst;
                        }
                        out.width = width;
                        break;
                    }
                    default:
                        return false;
                    }
                    return out.target.valid();
                }
                OutputBinding base;
                if (!self(self, range->value(), portWidth, base))
                {
                    return false;
                }
                ExprNodeId left = connectionLowerer_.lowerExpression(range->left());
                ExprNodeId right = connectionLowerer_.lowerExpression(range->right());
                auto leftConst = evalConstInt(plan_, lowering_, left);
                auto rightConst = evalConstInt(plan_, lowering_, right);
                out.target = base.target;
                out.location = range->sourceRange.start();
                out.isSlice = true;
                switch (range->getSelectionKind())
                {
                case slang::ast::RangeSelectionKind::Simple: {
                    if (!leftConst || !rightConst)
                    {
                        return false;
                    }
                    const int64_t lo = std::min(*leftConst, *rightConst);
                    const int64_t hi = std::max(*leftConst, *rightConst);
                    int64_t low = base.low;
                    if (lo > 0 &&
                        low > std::numeric_limits<int64_t>::max() - lo)
                    {
                        return false;
                    }
                    out.low = low + lo;
                    out.width = hi - lo + 1;
                    break;
                }
                case slang::ast::RangeSelectionKind::IndexedUp:
                case slang::ast::RangeSelectionKind::IndexedDown: {
                    if (!leftConst || !rightConst)
                    {
                        return false;
                    }
                    if (*rightConst <= 0)
                    {
                        return false;
                    }
                    const int64_t width = *rightConst;
                    int64_t low = base.low;
                    int64_t indexLow = 0;
                    if (range->getSelectionKind() ==
                        slang::ast::RangeSelectionKind::IndexedDown)
                    {
                        indexLow = *leftConst - width + 1;
                    }
                    else
                    {
                        indexLow = *leftConst;
                    }
                    if (indexLow > 0 &&
                        low > std::numeric_limits<int64_t>::max() - indexLow)
                    {
                        return false;
                    }
                    out.low = low + indexLow;
                    out.width = width;
                    break;
                }
                default:
                    return false;
                }
                return out.target.valid();
            }
            return false;
        };

        for (const auto& instanceInfo : plan_.instances)
        {
            if (!instanceInfo.instance)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->error(
                        slang::SourceLocation{},
                        "Skipping instance without symbol reference");
                }
                continue;
            }

            const slang::ast::InstanceSymbol& instance = *instanceInfo.instance;
            const slang::ast::InstanceBodySymbol& body = instance.body;
            std::vector<std::string> inputNames;
            std::vector<std::string> outputNames;
            std::vector<std::string> inoutNames;
            std::vector<wolvrix::lib::grh::ValueId> inputOperands;
            std::vector<wolvrix::lib::grh::ValueId> outputResults;
            std::vector<wolvrix::lib::grh::ValueId> inoutInOperands;
            std::vector<wolvrix::lib::grh::ValueId> inoutOutResults;
            std::vector<wolvrix::lib::grh::ValueId> inoutOeResults;
            bool ok = true;
            auto makeUnconnectedInput = [&](int64_t width,
                                            bool isSigned,
                                            wolvrix::lib::grh::ValueType valueType,
                                            slang::SourceLocation location) -> wolvrix::lib::grh::ValueId {
                const int64_t maxWidth = std::numeric_limits<int32_t>::max();
                const int32_t boundedWidth =
                    width > maxWidth ? std::numeric_limits<int32_t>::max()
                                     : static_cast<int32_t>(width);
                const int32_t normalizedWidth = normalizeWidth(boundedWidth);
                wolvrix::lib::grh::SymbolId sym =
                    makeInternalValueSymbol();
                wolvrix::lib::grh::ValueId result =
                    graph_.createValue(sym, normalizedWidth, isSigned, valueType);
                wolvrix::lib::grh::OperationId op =
                    createOp(wolvrix::lib::grh::OperationKind::kConstant,
                             wolvrix::lib::grh::SymbolId::invalid(),
                             location);
                graph_.addResult(op, result);
                if (valueType == wolvrix::lib::grh::ValueType::Real)
                {
                    graph_.setAttr(op, "constValue", std::string("0.0"));
                }
                else if (valueType == wolvrix::lib::grh::ValueType::String)
                {
                    graph_.setAttr(op, "constValue", std::string());
                }
                else
                {
                    graph_.setAttr(op, "constValue",
                                   std::to_string(normalizedWidth) + "'bx");
                }
                return result;
            };

            for (const slang::ast::Symbol* portSymbol : body.getPortList())
            {
                if (!portSymbol)
                {
                    continue;
                }
                const auto* port = portSymbol->as_if<slang::ast::PortSymbol>();
                if (!port || port->name.empty() || port->isNullPort)
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->error(
                            portSymbol->location,
                            "Skipping instance with unsupported port declaration");
                    }
                    ok = false;
                    break;
                }

                const slang::ast::PortConnection* connection =
                    instance.getPortConnection(*port);
                const slang::ast::Expression* expr =
                    connection ? connection->getExpression() : nullptr;
                const wolvrix::lib::grh::ValueType portValueType =
                    classifyValueType(port->getType());

                switch (port->direction)
                {
                case slang::ast::ArgumentDirection::In: {
                    const int64_t portWidth =
                        static_cast<int64_t>(port->getType().getBitstreamWidth());
                    const bool portSigned = port->getType().isSigned();
                    if (!expr)
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                port->location,
                                "Leaving unconnected input port on instance");
                        }
                        inputNames.emplace_back(port->name);
                        inputOperands.push_back(
                            makeUnconnectedInput(portWidth, portSigned, portValueType,
                                                 port->location));
                        break;
                    }
                    if (expr->bad())
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance with missing port connection");
                        }
                        ok = false;
                        break;
                    }
                    inputNames.emplace_back(port->name);
                    wolvrix::lib::grh::ValueId value = emitConnectionExpr(*expr);
                    if (!value.valid())
                    {
                        ok = false;
                        break;
                    }
                    inputOperands.push_back(value);
                    break;
                }
                case slang::ast::ArgumentDirection::Out: {
                    const int64_t portWidth =
                        static_cast<int64_t>(port->getType().getBitstreamWidth());
                    const bool portSigned = port->getType().isSigned();
                    if (!expr || expr->bad())
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->warn(
                                port->location,
                                "Skipping unconnected output port on instance");
                        }
                        outputNames.emplace_back(port->name);
                        const int64_t width = portWidth > 0 ? portWidth : 1;
                        wolvrix::lib::grh::SymbolId tempSym =
                            makeInternalValueSymbol();
                        wolvrix::lib::grh::ValueId tempValue =
                            graph_.createValue(tempSym, width, portSigned, portValueType);
                        outputResults.push_back(tempValue);
                        break;
                    }
                    outputNames.emplace_back(port->name);
                    OutputBinding binding;
                    if (!resolveOutputBinding(resolveOutputBinding, *expr, portWidth, binding))
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance with unsupported output connection");
                        }
                        ok = false;
                        break;
                    }
                    if (binding.isXmr)
                    {
                        if (binding.low != 0)
                        {
                            if (context_.diagnostics)
                            {
                                context_.diagnostics->error(
                                    port->location,
                                    "Skipping instance output connection to sliced XMR target");
                            }
                            ok = false;
                            break;
                        }
                        wolvrix::lib::grh::SymbolId tempSym =
                            makeInternalValueSymbol();
                        wolvrix::lib::grh::ValueId tempValue =
                            graph_.createValue(tempSym, binding.width, portSigned, portValueType);
                        outputResults.push_back(tempValue);
                        instanceXmrWrites_.push_back(
                            InstanceXmrWrite{std::move(binding.xmrPath), tempValue,
                                             binding.location});
                        break;
                    }
                    if (resolveInoutBinding(binding.target))
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance output connection to inout port");
                        }
                        ok = false;
                        break;
                    }
                    wolvrix::lib::grh::ValueId targetValue = valueForSymbol(binding.target);
                    if (!targetValue.valid())
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance with missing output binding");
                        }
                        ok = false;
                        break;
                    }
                    const int64_t targetWidth = graph_.getValue(targetValue).width();
                    if (binding.low < 0 || binding.width <= 0 ||
                        (targetWidth > 0 && binding.low + binding.width > targetWidth &&
                         (binding.isSlice || binding.low != 0)))
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance with out-of-range output slice");
                        }
                        ok = false;
                        break;
                    }
                    wolvrix::lib::grh::SymbolId tempSym =
                        makeInternalValueSymbol();
                    wolvrix::lib::grh::ValueId tempValue =
                        graph_.createValue(tempSym, binding.width, portSigned, portValueType);
                    outputResults.push_back(tempValue);
                    instanceSliceWrites_[binding.target.index].push_back(
                        InstanceSliceWrite{binding.target, binding.low,
                                           binding.width, tempValue,
                                           binding.location, binding.isSlice});
                    break;
                }
                case slang::ast::ArgumentDirection::InOut: {
                    if (!expr || expr->bad())
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance with missing port connection");
                        }
                        ok = false;
                        break;
                    }
                    inoutNames.emplace_back(port->name);
                    const PortInfo::InoutBinding* inoutBinding = resolveInoutBinding(*expr);
                    if (!inoutBinding)
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance with unsupported inout connection");
                        }
                        ok = false;
                        break;
                    }
                    const auto& binding = *inoutBinding;
                    wolvrix::lib::grh::ValueId inValue = valueForSymbol(binding.inSymbol);
                    wolvrix::lib::grh::ValueId outValue = valueForSymbol(binding.outSymbol);
                    wolvrix::lib::grh::ValueId oeValue = valueForSymbol(binding.oeSymbol);
                    if (!inValue.valid() || !outValue.valid() || !oeValue.valid())
                    {
                        if (context_.diagnostics)
                        {
                            context_.diagnostics->error(
                                port->location,
                                "Skipping instance with incomplete inout binding");
                        }
                        ok = false;
                        break;
                    }
                    inoutInOperands.push_back(inValue);
                    inoutOutResults.push_back(outValue);
                    inoutOeResults.push_back(oeValue);
                    break;
                }
                default:
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->error(
                            port->location,
                            "Skipping instance with unsupported port direction");
                    }
                    ok = false;
                    break;
                }

                if (!ok)
                {
                    break;
                }
            }

            if (!ok)
            {
                continue;
            }

            std::vector<wolvrix::lib::grh::ValueId> operands;
            std::vector<wolvrix::lib::grh::ValueId> results;
            operands.reserve(inputOperands.size() + inoutInOperands.size());
            operands.insert(operands.end(), inputOperands.begin(), inputOperands.end());
            operands.insert(operands.end(), inoutInOperands.begin(), inoutInOperands.end());
            results.reserve(outputResults.size() + inoutOutResults.size() + inoutOeResults.size());
            results.insert(results.end(), outputResults.begin(), outputResults.end());
            results.insert(results.end(), inoutOutResults.begin(), inoutOutResults.end());
            results.insert(results.end(), inoutOeResults.begin(), inoutOeResults.end());

            const std::string moduleNameText =
                instanceInfo.moduleSymbol.valid()
                    ? std::string(plan_.symbolTable.text(instanceInfo.moduleSymbol))
                    : std::string();
            std::string moduleName = moduleNameText;
            if (!instanceInfo.isBlackbox)
            {
                PlanKey childKey;
                childKey.definition = &body.getDefinition();
                childKey.body = &body;
                childKey.paramSignature = instanceInfo.paramSignature;
                moduleName = assembler_.resolveGraphName(childKey, moduleNameText);
            }
            if (moduleName.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->error(
                        instance.location,
                        "Skipping instance with empty module name");
                }
                continue;
            }

            wolvrix::lib::grh::OperationKind kind = instanceInfo.isBlackbox
                                              ? wolvrix::lib::grh::OperationKind::kBlackbox
                                              : wolvrix::lib::grh::OperationKind::kInstance;
            wolvrix::lib::grh::OperationId op =
                createOp(kind, wolvrix::lib::grh::SymbolId::invalid(), instance.location);
            for (const auto& operand : operands)
            {
                graph_.addOperand(op, operand);
            }
            for (const auto& result : results)
            {
                graph_.addResult(op, result);
            }
            graph_.setAttr(op, "moduleName", moduleName);
            graph_.setAttr(op, "inputPortName", inputNames);
            graph_.setAttr(op, "outputPortName", outputNames);
            graph_.setAttr(op, "inoutPortName", inoutNames);

            if (instanceInfo.instanceSymbol.valid())
            {
                std::string_view name = plan_.symbolTable.text(instanceInfo.instanceSymbol);
                if (!name.empty())
                {
                    graph_.setAttr(op, "instanceName", std::string(name));
                }
            }

            if (instanceInfo.isBlackbox && !instanceInfo.parameters.empty())
            {
                std::vector<std::string> paramNames;
                std::vector<std::string> paramValues;
                paramNames.reserve(instanceInfo.parameters.size());
                paramValues.reserve(instanceInfo.parameters.size());
                for (const auto& param : instanceInfo.parameters)
                {
                    if (!param.symbol.valid())
                    {
                        continue;
                    }
                    std::string_view name = plan_.symbolTable.text(param.symbol);
                    if (name.empty())
                    {
                        continue;
                    }
                    paramNames.emplace_back(name);
                    paramValues.emplace_back(param.value);
                }
                if (!paramNames.empty() && paramNames.size() == paramValues.size())
                {
                    graph_.setAttr(op, "parameterNames", paramNames);
                    graph_.setAttr(op, "parameterValues", paramValues);
                }
            }
        }
    }

    void emitInstanceSliceWrites()
    {
        if (instanceSliceWrites_.empty() && instanceXmrWrites_.empty())
        {
            return;
        }

        auto makeSliceStatic = [&](wolvrix::lib::grh::ValueId base,
                                   int64_t low,
                                   int64_t high,
                                   slang::SourceLocation location) -> wolvrix::lib::grh::ValueId {
            const int64_t width = high - low + 1;
            if (width <= 0)
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kSliceStatic,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         location);
            graph_.addOperand(op, base);
            graph_.setAttr(op, "sliceStart", low);
            graph_.setAttr(op, "sliceEnd", high);
            wolvrix::lib::grh::SymbolId sym =
                makeInternalValueSymbol();
            const wolvrix::lib::grh::ValueType baseType = graph_.getValue(base).type();
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, width, false, baseType);
            graph_.addResult(op, result);
            return result;
        };
        auto makeZero = [&](int64_t width,
                            slang::SourceLocation location) -> wolvrix::lib::grh::ValueId {
            if (width <= 0)
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            wolvrix::lib::grh::SymbolId sym =
                makeInternalValueSymbol();
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, static_cast<int32_t>(width), false,
                                   wolvrix::lib::grh::ValueType::Logic);
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kConstant,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         location);
            graph_.addResult(op, result);
            graph_.setAttr(op, "constValue",
                           std::to_string(width) + "'b0");
            return result;
        };
        auto makeReplicate = [&](wolvrix::lib::grh::ValueId value,
                                 int64_t count,
                                 slang::SourceLocation location) -> wolvrix::lib::grh::ValueId {
            if (!value.valid() || count <= 0)
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            const int64_t valueWidth = graph_.getValue(value).width();
            if (valueWidth <= 0)
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            const int64_t width = valueWidth * count;
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kReplicate,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         location);
            graph_.addOperand(op, value);
            graph_.setAttr(op, "rep", count);
            wolvrix::lib::grh::SymbolId sym =
                makeInternalValueSymbol();
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, static_cast<int32_t>(width),
                                   graph_.getValue(value).isSigned(),
                                   graph_.getValue(value).type());
            graph_.addResult(op, result);
            return result;
        };
        auto resizeValueToWidth = [&](wolvrix::lib::grh::ValueId value,
                                      int64_t targetWidth,
                                      bool signExtend,
                                      slang::SourceLocation location)
            -> wolvrix::lib::grh::ValueId {
            if (!value.valid() || targetWidth <= 0)
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            const int64_t sourceWidth = graph_.getValue(value).width();
            if (sourceWidth <= 0 || sourceWidth == targetWidth)
            {
                return value;
            }
            if (targetWidth < sourceWidth)
            {
                return makeSliceStatic(value, 0, targetWidth - 1, location);
            }
            const int64_t padWidth = targetWidth - sourceWidth;
            if (padWidth <= 0)
            {
                return value;
            }
            wolvrix::lib::grh::ValueId padValue = wolvrix::lib::grh::ValueId::invalid();
            if (signExtend && sourceWidth > 0)
            {
                wolvrix::lib::grh::ValueId signBit =
                    makeSliceStatic(value, sourceWidth - 1, sourceWidth - 1, location);
                padValue = makeReplicate(signBit, padWidth, location);
            }
            else
            {
                padValue = makeZero(padWidth, location);
            }
            if (!padValue.valid())
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kConcat,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         location);
            graph_.addOperand(op, padValue);
            graph_.addOperand(op, value);
            wolvrix::lib::grh::SymbolId sym =
                makeInternalValueSymbol();
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, static_cast<int32_t>(targetWidth),
                                   signExtend, graph_.getValue(value).type());
            graph_.addResult(op, result);
            return result;
        };
        auto emitAggregateLaneSlice =
            [&](wolvrix::lib::grh::ValueId value,
                int64_t lane,
                int64_t laneWidth,
                wolvrix::lib::grh::ValueType valueType,
                slang::SourceLocation location) -> wolvrix::lib::grh::ValueId {
            if (!value.valid() || lane < 0 || laneWidth <= 0)
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            const int64_t low = lane * laneWidth;
            const int64_t high = low + laneWidth - 1;
            return createStaticSliceValue(value, low, high, false, valueType, location);
        };
        auto replaceWholePackedAggregateReads =
            [&](PlanSymbolId target,
                std::vector<InstanceSliceWrite>& writes,
                wolvrix::lib::grh::ValueId targetValue,
                int64_t targetWidth,
                slang::SourceLocation location) -> bool {
            if (!target.valid() || writes.size() != 1 || !writes.front().value.valid())
            {
                return false;
            }
            InstanceSliceWrite& write = writes.front();
            if (write.explicitSlice || write.low != 0 || write.width != targetWidth)
            {
                return false;
            }
            const SignalId signalId = findSignalId(target);
            if (signalId == kInvalidPlanIndex)
            {
                return false;
            }
            const SignalInfo& signal = plan_.signals[signalId];
            if (!isPackedAggregateVariable(signal) || signal.memoryRows <= 0 ||
                !isMemoryBackedPackedAggregate(target))
            {
                return false;
            }
            const int64_t laneWidth = signal.width > 0 ? signal.width : 1;
            if (targetWidth != laneWidth * signal.memoryRows)
            {
                return false;
            }
            const wolvrix::lib::grh::OperationId targetDef =
                graph_.getValue(targetValue).definingOp();
            if (!targetDef.valid() ||
                graph_.getOperation(targetDef).kind() !=
                    wolvrix::lib::grh::OperationKind::kConcat)
            {
                return false;
            }

            bool replaced = false;
            for (const auto& read : emittedMemoryReads_)
            {
                if (read.memory.index != target.index || read.signal != signalId ||
                    !read.value.valid() || read.isSync)
                {
                    continue;
                }
                const auto address = evalConstInt(plan_, lowering_, read.address);
                if (!address || *address < 0 || *address >= signal.memoryRows)
                {
                    continue;
                }
                wolvrix::lib::grh::ValueId laneValue =
                    emitAggregateLaneSlice(write.value, *address, laneWidth,
                                           signal.valueType, location);
                if (!laneValue.valid())
                {
                    continue;
                }
                graph_.replaceAllUses(read.value, laneValue);
                if (read.expr != kInvalidPlanIndex &&
                    read.expr < static_cast<ExprNodeId>(valueByExpr_.size()))
                {
                    valueByExpr_[read.expr] = laneValue;
                }
                replaced = true;
            }

            if (!replaced)
            {
                return false;
            }

            wolvrix::lib::grh::ValueId fullValue =
                resizeValueToWidth(write.value, targetWidth,
                                   graph_.getValue(write.value).isSigned(), location);
            if (fullValue.valid())
            {
                graph_.replaceAllUses(targetValue, fullValue);
                valueBySymbol_[target.index] = fullValue;
                if (target.index < packedAggregateWholeReadBySymbol_.size() &&
                    packedAggregateWholeReadBySymbol_[target.index] == targetValue)
                {
                    packedAggregateWholeReadBySymbol_[target.index] = fullValue;
                }
            }
            instanceMergedTargets_.insert(target.index);
            return true;
        };

        for (auto& [targetIndex, writes] : instanceSliceWrites_)
        {
            if (writes.empty())
            {
                continue;
            }
            slang::SourceLocation writeLoc = writes.front().location;
            PlanSymbolId target;
            target.index = targetIndex;
            wolvrix::lib::grh::ValueId targetValue = valueForSymbol(target);
            if (!targetValue.valid())
            {
                continue;
            }
            const int64_t targetWidth = graph_.getValue(targetValue).width();
            if (targetWidth <= 0)
            {
                continue;
            }
            if (replaceWholePackedAggregateReads(target, writes, targetValue,
                                                 targetWidth, writeLoc))
            {
                continue;
            }
            wolvrix::lib::grh::ValueId baseValue = targetValue;
            int64_t baseWidth = targetWidth;
            bool hasWritebackBase = false;
            for (const auto& entry : writeBack_.entries)
            {
                if (entry.target.index != targetIndex ||
                    entry.domain != ControlDomain::Combinational)
                {
                    continue;
                }
                wolvrix::lib::grh::ValueId mergedBase = emitExpr(entry.nextValue);
                if (mergedBase.valid())
                {
                    baseValue = mergedBase;
                    baseWidth = graph_.getValue(baseValue).width();
                    instanceMergedTargets_.insert(targetIndex);
                    hasWritebackBase = true;
                }
                break;
            }
            if (baseWidth != targetWidth)
            {
                baseValue = targetValue;
                baseWidth = targetWidth;
            }

            const bool hasOtherAssignments = hasWritebackBase || writes.size() > 1;
            if (targetWidth > 0)
            {
                for (auto& write : writes)
                {
                    if (!write.explicitSlice && write.low == 0)
                    {
                        if (write.width > targetWidth)
                        {
                            write.width = targetWidth;
                        }
                        else if (write.width < targetWidth && !hasOtherAssignments)
                        {
                            write.width = targetWidth;
                        }
                    }
                }
            }

            std::sort(writes.begin(), writes.end(),
                      [](const InstanceSliceWrite& lhs, const InstanceSliceWrite& rhs) {
                          return lhs.low < rhs.low;
                      });

            bool overlap = false;
            int64_t cursor = 0;
            struct Segment {
                int64_t low = 0;
                int64_t high = 0;
                wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
                bool fromBase = false;
            };
            std::vector<Segment> segments;
            for (const auto& write : writes)
            {
                if (write.low < cursor || write.low + write.width > targetWidth)
                {
                    overlap = true;
                    break;
                }
                if (write.low > cursor)
                {
                    segments.push_back(Segment{cursor, write.low - 1,
                                               baseValue, true});
                }
                segments.push_back(Segment{write.low,
                                           write.low + write.width - 1,
                                           write.value, false});
                cursor = write.low + write.width;
            }
            if (overlap)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->error(
                        writes.front().location,
                        "Skipping instance output merge with overlapping slices");
                }
                continue;
            }
            if (cursor < targetWidth)
            {
                segments.push_back(Segment{cursor, targetWidth - 1,
                                           baseValue, true});
            }

            std::vector<wolvrix::lib::grh::ValueId> concatOperands;
            concatOperands.reserve(segments.size());
            for (auto it = segments.rbegin(); it != segments.rend(); ++it)
            {
                const Segment& seg = *it;
                wolvrix::lib::grh::ValueId segValue = seg.value;
                const int64_t segWidth = seg.high - seg.low + 1;
                if (seg.fromBase)
                {
                    if (!(seg.low == 0 && seg.high == baseWidth - 1))
                    {
                        segValue = makeSliceStatic(baseValue, seg.low, seg.high, writeLoc);
                    }
                }
                else
                {
                    const int64_t valWidth = graph_.getValue(segValue).width();
                    if (valWidth != segWidth)
                    {
                        const bool signExtend = graph_.getValue(segValue).isSigned();
                        segValue = resizeValueToWidth(segValue, segWidth, signExtend, writeLoc);
                        if (!segValue.valid())
                        {
                            if (context_.diagnostics)
                            {
                                context_.diagnostics->error(
                                    writes.front().location,
                                    "Skipping instance output merge with width mismatch");
                            }
                            concatOperands.clear();
                            break;
                        }
                    }
                }
                if (!segValue.valid())
                {
                    concatOperands.clear();
                    break;
                }
                concatOperands.push_back(segValue);
            }
            if (concatOperands.empty())
            {
                continue;
            }

            wolvrix::lib::grh::ValueId merged = concatOperands.front();
            if (concatOperands.size() > 1)
            {
                wolvrix::lib::grh::OperationId op =
                    createOp(wolvrix::lib::grh::OperationKind::kConcat,
                             wolvrix::lib::grh::SymbolId::invalid(),
                             writeLoc);
                for (const auto& operand : concatOperands)
                {
                    graph_.addOperand(op, operand);
                }
                wolvrix::lib::grh::SymbolId sym =
                    makeInternalValueSymbol();
                wolvrix::lib::grh::ValueId result =
                    graph_.createValue(sym, targetWidth, false,
                                       graph_.getValue(targetValue).type());
                graph_.addResult(op, result);
                merged = result;
            }

            wolvrix::lib::grh::OperationId assignOp =
                createOp(wolvrix::lib::grh::OperationKind::kAssign,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         writeLoc);
            graph_.addOperand(assignOp, merged);
            graph_.addResult(assignOp, targetValue);
        }

        for (auto& write : instanceXmrWrites_)
        {
            if (write.xmrPath.empty() || !write.value.valid())
            {
                continue;
            }
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kXMRWrite,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         write.location);
            graph_.addOperand(op, write.value);
            graph_.setAttr(op, "xmrPath", write.xmrPath);
        }
    }

    void emitXmrWrites()
    {
        for (const auto& stmt : lowering_.loweredStmts)
        {
            if (stmt.kind != LoweredStmtKind::Write)
            {
                continue;
            }
            const WriteIntent& write = stmt.write;
            if (!write.isXmr)
            {
                continue;
            }
            if (write.xmrPath.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(write.location,
                                               "Skipping XMR write without path");
                }
                continue;
            }
            if (write.domain != ControlDomain::Combinational || !stmt.eventEdges.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(write.location,
                                               "Skipping sequential XMR write");
                }
                continue;
            }
            if (stmt.updateCond != kInvalidPlanIndex)
            {
                auto cond = evalConstInt(plan_, lowering_, stmt.updateCond);
                if (!cond || *cond == 0)
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->warn(write.location,
                                                   "Skipping conditional XMR write");
                    }
                    continue;
                }
            }
            wolvrix::lib::grh::ValueId data = emitExpr(write.value);
            if (!data.valid())
            {
                continue;
            }
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kXMRWrite,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         write.location);
            graph_.addOperand(op, data);
            graph_.setAttr(op, "xmrPath", write.xmrPath);
        }
    }

    wolvrix::lib::grh::ValueId emitConnectionExpr(const slang::ast::Expression& expr)
    {
        ExprNodeId id = connectionLowerer_.lowerExpression(expr);
        if (id == kInvalidPlanIndex)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        ensureConnectionMemoryRead(id, expr.sourceRange.start());
        return emitExpr(id);
    }

    void ensureConnectionMemoryRead(ExprNodeId id, slang::SourceLocation location)
    {
        if (id == kInvalidPlanIndex || id >= memoryReadIndexByExpr_.size())
        {
            return;
        }
        if (memoryReadIndexByExpr_[id] != kInvalidMemoryReadIndex)
        {
            return;
        }

        struct SelectStep
        {
            ExprNodeId node = kInvalidPlanIndex;
            ExprNodeId index = kInvalidPlanIndex;
        };
        ExprNodeId current = id;
        std::vector<SelectStep> selectSteps;
        while (current != kInvalidPlanIndex && current < lowering_.values.size())
        {
            const ExprNode& node = lowering_.values[current];
            if (node.kind != ExprNodeKind::Operation ||
                node.operands.size() < 2)
            {
                break;
            }
            const bool isSliceAddress =
                node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic ||
                node.op == wolvrix::lib::grh::OperationKind::kSliceArray;
            if (!isSliceAddress)
            {
                break;
            }
            selectSteps.push_back(SelectStep{current, node.operands[1]});
            current = node.operands[0];
        }
        if (current == kInvalidPlanIndex || current >= lowering_.values.size())
        {
            return;
        }
        const ExprNode& baseNode = lowering_.values[current];
        if (baseNode.kind != ExprNodeKind::Symbol)
        {
            return;
        }

        SignalId signal = kInvalidPlanIndex;
        for (SignalId i = 0; i < static_cast<SignalId>(plan_.signals.size()); ++i)
        {
            const auto& info = plan_.signals[i];
            if (info.symbol.index == baseNode.symbol.index &&
                info.memoryRows > 0 &&
                info.kind != SignalKind::Net)
            {
                signal = i;
                break;
            }
        }
        if (signal == kInvalidPlanIndex)
        {
            return;
        }

        if (isPackedAggregateVariable(plan_.signals[signal]))
        {
            if (!isMemoryBackedPackedAggregate(baseNode.symbol))
            {
                return;
            }
            if (selectSteps.empty())
            {
                return;
            }
            const SelectStep& dataStep = selectSteps.front();
            if (dataStep.node != kInvalidPlanIndex &&
                dataStep.node < static_cast<ExprNodeId>(lowering_.values.size()))
            {
                const ExprNode& dataNode = lowering_.values[dataStep.node];
                if (dataNode.kind == ExprNodeKind::Operation &&
                    dataNode.op == wolvrix::lib::grh::OperationKind::kSliceDynamic)
                {
                    return;
                }
            }
        }

        if (!selectSteps.empty())
        {
            std::reverse(selectSteps.begin(), selectSteps.end());
        }

        const auto& dims = plan_.signals[signal].unpackedDims;
        const std::size_t addressCount = dims.empty() ? 1 : dims.size();
        if (selectSteps.size() < addressCount)
        {
            return;
        }
        std::vector<ExprNodeId> indices;
        indices.reserve(addressCount);
        for (std::size_t i = 0; i < addressCount; ++i)
        {
            indices.push_back(selectSteps[i].index);
        }
        const ExprNodeId dataId = selectSteps[addressCount - 1].node;
        if (dataId == kInvalidPlanIndex ||
            dataId >= static_cast<ExprNodeId>(memoryReadIndexByExpr_.size()))
        {
            return;
        }
        if (memoryReadIndexByExpr_[dataId] != kInvalidMemoryReadIndex)
        {
            return;
        }

        ExprNodeId address = kInvalidPlanIndex;
        if (dims.empty())
        {
            address = indices.front();
        }
        else
        {
            auto addExprNode = [&](ExprNode node) -> ExprNodeId {
                const ExprNodeId newId =
                    static_cast<ExprNodeId>(lowering_.values.size());
                lowering_.values.push_back(std::move(node));
                registerExprNode();
                return newId;
            };
            auto addConst = [&](int64_t literal) -> ExprNodeId {
                ExprNode node;
                node.kind = ExprNodeKind::Constant;
                node.literal = std::to_string(literal);
                node.location = location;
                return addExprNode(std::move(node));
            };
            auto addOp = [&](wolvrix::lib::grh::OperationKind op,
                             std::vector<ExprNodeId> ops) -> ExprNodeId {
                for (ExprNodeId opId : ops)
                {
                    if (opId == kInvalidPlanIndex)
                    {
                        return kInvalidPlanIndex;
                    }
                }
                ExprNode node;
                node.kind = ExprNodeKind::Operation;
                node.op = op;
                node.operands = std::move(ops);
                node.location = location;
                return addExprNode(std::move(node));
            };
            auto buildLinearAddress =
                [&](const std::vector<ExprNodeId>& idx,
                    const std::vector<UnpackedDimInfo>& dimsIn) -> ExprNodeId {
                if (idx.size() < dimsIn.size())
                {
                    return kInvalidPlanIndex;
                }
                int64_t stride = 1;
                std::vector<int64_t> strides(dimsIn.size(), 1);
                for (std::size_t i = dimsIn.size(); i-- > 0;)
                {
                    strides[i] = stride;
                    const int32_t extent = dimsIn[i].extent;
                    if (extent > 0 &&
                        stride <= std::numeric_limits<int64_t>::max() / extent)
                    {
                        stride *= extent;
                    }
                }
                ExprNodeId addressId = kInvalidPlanIndex;
                for (std::size_t i = 0; i < dimsIn.size(); ++i)
                {
                    ExprNodeId term = idx[i];
                    const UnpackedDimInfo& dim = dimsIn[i];
                    if (dim.left < dim.right)
                    {
                        if (dim.left != 0)
                        {
                            ExprNodeId offset = addConst(dim.left);
                            term = addOp(wolvrix::lib::grh::OperationKind::kSub,
                                         {term, offset});
                        }
                    }
                    else
                    {
                        ExprNodeId offset = addConst(dim.left);
                        term = addOp(wolvrix::lib::grh::OperationKind::kSub,
                                     {offset, term});
                    }
                    if (strides[i] != 1)
                    {
                        ExprNodeId strideId = addConst(strides[i]);
                        term = addOp(wolvrix::lib::grh::OperationKind::kMul,
                                     {term, strideId});
                    }
                    if (addressId == kInvalidPlanIndex)
                    {
                        addressId = term;
                    }
                    else
                    {
                        addressId = addOp(wolvrix::lib::grh::OperationKind::kAdd,
                                          {addressId, term});
                    }
                }
                return addressId;
            };
            address = buildLinearAddress(indices, dims);
            if (address == kInvalidPlanIndex)
            {
                return;
            }
        }

        if (address == kInvalidPlanIndex)
        {
            return;
        }

        MemoryReadPort entry;
        entry.memory = baseNode.symbol;
        entry.signal = signal;
        entry.address = address;
        entry.data = dataId;
        entry.replacement = dataId;
        entry.isSync = false;
        entry.location = location;

        const int32_t index = static_cast<int32_t>(lowering_.memoryReads.size());
        lowering_.memoryReads.push_back(std::move(entry));
        memoryReadIndexByExpr_[dataId] = index;
    }

    PlanSymbolId resolveSimpleSymbol(const slang::ast::Expression& expr) const
    {
        if (const auto* assign = expr.as_if<slang::ast::AssignmentExpression>())
        {
            if (assign->isLValueArg())
            {
                return resolveSimpleSymbol(assign->left());
            }
            return resolveSimpleSymbol(assign->right());
        }
        if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
        {
            return lookupPlanSignal(plan_, named->symbol);
        }
        if (const auto* hier = expr.as_if<slang::ast::HierarchicalValueExpression>())
        {
            (void)hier;
            return {};
        }
        if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
        {
            return resolveSimpleSymbol(conversion->operand());
        }
        return {};
    }

    const PortInfo::InoutBinding* resolveInoutBinding(
        const slang::ast::Expression& expr) const
    {
        PlanSymbolId symbol = resolveSimpleSymbol(expr);
        return resolveInoutBinding(symbol);
    }

    const PortInfo::InoutBinding* resolveInoutBinding(PlanSymbolId symbol) const
    {
        if (!symbol.valid())
        {
            return nullptr;
        }
        std::string_view name = plan_.symbolTable.text(symbol);
        if (name.empty())
        {
            return nullptr;
        }
        const PortInfo* port = findPortByName(plan_, name);
        if (!port)
        {
            port = findPortByInoutName(plan_, name);
        }
        if (port && port->direction == PortDirection::Inout && port->inoutSymbol)
        {
            return &(*port->inoutSymbol);
        }
        if (const InoutSignalInfo* info = findInoutSignalInfo(symbol))
        {
            return &info->binding;
        }
        return nullptr;
    }

    void registerExprNode()
    {
        valueByExpr_.push_back(wolvrix::lib::grh::ValueId::invalid());
        memoryReadIndexByExpr_.push_back(kInvalidMemoryReadIndex);
    }

    class ConnectionExprLowerer {
    public:
        explicit ConnectionExprLowerer(GraphAssemblyState& state)
            : state_(state)
        {
        }

        ExprNodeId adjustPackedIndex(const slang::ast::Type* baseType,
                                     ExprNodeId index,
                                     slang::SourceLocation location)
        {
            if (!baseType || index == kInvalidPlanIndex)
            {
                return index;
            }
            const auto& canonical = baseType->getCanonicalType();
            if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
            {
                return index;
            }
            const auto& packed = canonical.as<slang::ast::PackedArrayType>();
            const int64_t offset =
                std::min<int64_t>(packed.range.left, packed.range.right);
            uint64_t elementWidthRaw = packed.elementType.getBitstreamWidth();
            if (elementWidthRaw == 0)
            {
                elementWidthRaw = 1;
            }
            constexpr uint64_t maxWidth =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            const int64_t elementWidth =
                elementWidthRaw > maxWidth ? static_cast<int64_t>(maxWidth)
                                           : static_cast<int64_t>(elementWidthRaw);
            int32_t indexWidthHint = 0;
            if (index < state_.lowering_.values.size())
            {
                indexWidthHint = state_.lowering_.values[index].widthHint;
            }
            if (indexWidthHint <= 0)
            {
                indexWidthHint = 32;
            }
            ExprNodeId adjustedId = index;
            if (offset != 0)
            {
                ExprNode offsetNode;
                offsetNode.kind = ExprNodeKind::Constant;
                offsetNode.literal = std::to_string(offset);
                offsetNode.location = location;
                offsetNode.widthHint = indexWidthHint;
                ExprNodeId offsetId = addSyntheticNode(std::move(offsetNode));
                ExprNode subNode;
                subNode.kind = ExprNodeKind::Operation;
                subNode.op = wolvrix::lib::grh::OperationKind::kSub;
                subNode.operands = {adjustedId, offsetId};
                subNode.location = location;
                subNode.widthHint = indexWidthHint;
                adjustedId = addSyntheticNode(std::move(subNode));
            }
            if (elementWidth > 1)
            {
                int32_t elementWidthHint = 0;
                for (int64_t temp = elementWidth; temp > 0; temp >>= 1)
                {
                    ++elementWidthHint;
                }
                if (elementWidthHint <= 0)
                {
                    elementWidthHint = 1;
                }
                const int32_t constWidthHint =
                    elementWidthHint > indexWidthHint ? elementWidthHint : indexWidthHint;
                int64_t mulWidthHint =
                    static_cast<int64_t>(indexWidthHint) + elementWidthHint;
                if (mulWidthHint > std::numeric_limits<int32_t>::max())
                {
                    mulWidthHint = std::numeric_limits<int32_t>::max();
                }
                ExprNode widthNode;
                widthNode.kind = ExprNodeKind::Constant;
                widthNode.literal = std::to_string(elementWidth);
                widthNode.location = location;
                widthNode.widthHint = constWidthHint;
                ExprNodeId widthId = addSyntheticNode(std::move(widthNode));
                const int32_t mulWidth = static_cast<int32_t>(mulWidthHint);
                adjustedId = resizeValueToWidth(adjustedId, mulWidth, false,
                                                location);
                widthId = resizeValueToWidth(widthId, mulWidth, false,
                                             location);
                ExprNode mulNode;
                mulNode.kind = ExprNodeKind::Operation;
                mulNode.op = wolvrix::lib::grh::OperationKind::kMul;
                mulNode.operands = {adjustedId, widthId};
                mulNode.location = location;
                mulNode.widthHint = mulWidth;
                adjustedId = addSyntheticNode(std::move(mulNode));
            }
            uint64_t bitstreamWidth = canonical.getBitstreamWidth();
            if (bitstreamWidth > 1)
            {
                uint64_t temp = bitstreamWidth - 1;
                int32_t clampWidth = 0;
                while (temp > 0 && clampWidth < std::numeric_limits<int32_t>::max())
                {
                    ++clampWidth;
                    temp >>= 1;
                }
                if (clampWidth > 0)
                {
                    ExprNode zeroNode;
                    zeroNode.kind = ExprNodeKind::Constant;
                    zeroNode.literal = "0";
                    zeroNode.location = location;
                    zeroNode.widthHint = clampWidth;
                    ExprNodeId zeroId = addSyntheticNode(std::move(zeroNode));
                    ExprNode sliceNode;
                    sliceNode.kind = ExprNodeKind::Operation;
                    sliceNode.op = wolvrix::lib::grh::OperationKind::kSliceDynamic;
                    sliceNode.operands = {adjustedId, zeroId};
                    sliceNode.location = location;
                    sliceNode.widthHint = clampWidth;
                    adjustedId = addSyntheticNode(std::move(sliceNode));
                }
            }
            return adjustedId;
        }

        ExprNodeId adjustPackedElementLaneIndex(const slang::ast::Type* baseType,
                                                ExprNodeId index,
                                                slang::SourceLocation location)
        {
            if (!baseType || index == kInvalidPlanIndex)
            {
                return index;
            }
            const auto& canonical = baseType->getCanonicalType();
            if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
            {
                return index;
            }
            const auto& packed = canonical.as<slang::ast::PackedArrayType>();
            int32_t indexWidthHint = 0;
            if (index < state_.lowering_.values.size())
            {
                indexWidthHint = state_.lowering_.values[index].widthHint;
            }
            if (indexWidthHint <= 0)
            {
                indexWidthHint = 32;
            }
            const int64_t low = std::min<int64_t>(packed.range.left, packed.range.right);
            const int64_t high = std::max<int64_t>(packed.range.left, packed.range.right);
            if (packed.range.left <= packed.range.right)
            {
                ExprNode constNode;
                constNode.kind = ExprNodeKind::Constant;
                constNode.literal = std::to_string(high);
                constNode.location = location;
                constNode.widthHint = indexWidthHint;
                ExprNodeId highId = addSyntheticNode(std::move(constNode));
                ExprNode subNode;
                subNode.kind = ExprNodeKind::Operation;
                subNode.op = wolvrix::lib::grh::OperationKind::kSub;
                subNode.operands = {highId, index};
                subNode.location = location;
                subNode.widthHint = indexWidthHint;
                return addSyntheticNode(std::move(subNode));
            }
            if (low == 0)
            {
                return index;
            }
            ExprNode constNode;
            constNode.kind = ExprNodeKind::Constant;
            constNode.literal = std::to_string(low);
            constNode.location = location;
            constNode.widthHint = indexWidthHint;
            ExprNodeId lowId = addSyntheticNode(std::move(constNode));
            ExprNode subNode;
            subNode.kind = ExprNodeKind::Operation;
            subNode.op = wolvrix::lib::grh::OperationKind::kSub;
            subNode.operands = {index, lowId};
            subNode.location = location;
            subNode.widthHint = indexWidthHint;
            return addSyntheticNode(std::move(subNode));
        }

        ExprNodeId scalePackedElementLaneIndex(ExprNodeId laneIndex,
                                               int64_t elementWidth,
                                               slang::SourceLocation location)
        {
            if (laneIndex == kInvalidPlanIndex || elementWidth <= 1)
            {
                return laneIndex;
            }
            int32_t indexWidthHint = 0;
            if (laneIndex < state_.lowering_.values.size())
            {
                indexWidthHint = state_.lowering_.values[laneIndex].widthHint;
            }
            if (indexWidthHint <= 0)
            {
                indexWidthHint = 32;
            }
            int32_t elementWidthHint = 0;
            for (int64_t temp = elementWidth; temp > 0; temp >>= 1)
            {
                ++elementWidthHint;
            }
            if (elementWidthHint <= 0)
            {
                elementWidthHint = 1;
            }
            const int32_t constWidthHint =
                std::max(indexWidthHint, elementWidthHint);
            int64_t mulWidthHint =
                static_cast<int64_t>(indexWidthHint) + elementWidthHint;
            if (mulWidthHint > std::numeric_limits<int32_t>::max())
            {
                mulWidthHint = std::numeric_limits<int32_t>::max();
            }
            ExprNode widthNode;
            widthNode.kind = ExprNodeKind::Constant;
            widthNode.literal = std::to_string(elementWidth);
            widthNode.location = location;
            widthNode.widthHint = constWidthHint;
            ExprNodeId widthId = addSyntheticNode(std::move(widthNode));
            ExprNode mulNode;
            mulNode.kind = ExprNodeKind::Operation;
            mulNode.op = wolvrix::lib::grh::OperationKind::kMul;
            mulNode.operands = {laneIndex, widthId};
            mulNode.location = location;
            mulNode.widthHint = static_cast<int32_t>(mulWidthHint);
            return addSyntheticNode(std::move(mulNode));
        }

        std::optional<std::pair<int64_t, int64_t>>
        getPackedArrayElementSlice(const slang::ast::Type* baseType) const
        {
            if (!baseType || !state_.plan_.body)
            {
                return std::nullopt;
            }
            const auto& canonical = baseType->getCanonicalType();
            if (canonical.kind != slang::ast::SymbolKind::PackedArrayType)
            {
                return std::nullopt;
            }
            const auto& packed = canonical.as<slang::ast::PackedArrayType>();
            const uint64_t widthRaw =
                computeFixedWidth(packed.elementType, *state_.plan_.body,
                                  state_.context_.diagnostics);
            if (widthRaw == 0)
            {
                return std::nullopt;
            }
            const uint64_t totalWidthRaw =
                computeFixedWidth(canonical, *state_.plan_.body,
                                  state_.context_.diagnostics);
            if (totalWidthRaw <= widthRaw || widthRaw <= 1)
            {
                return std::nullopt;
            }
            const int64_t elementWidth =
                widthRaw > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())
                    ? static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                    : static_cast<int64_t>(widthRaw);
            if (elementWidth <= 0)
            {
                return std::nullopt;
            }
            const int64_t offset =
                std::min<int64_t>(packed.range.left, packed.range.right);
            return std::make_pair(offset, elementWidth);
        }

        std::optional<int32_t> getPackedArrayElementWidth(const slang::ast::Type* baseType) const
        {
            auto packedElement = getPackedArrayElementSlice(baseType);
            if (!packedElement || packedElement->second <= 0)
            {
                return std::nullopt;
            }
            return packedElement->second >
                       static_cast<int64_t>(std::numeric_limits<int32_t>::max())
                       ? std::numeric_limits<int32_t>::max()
                       : static_cast<int32_t>(packedElement->second);
        }

        ExprNodeId lowerExpression(const slang::ast::Expression& expr)
        {
            if (auto it = lowered_.find(&expr); it != lowered_.end())
            {
                return it->second;
            }

            auto paramLiteral = [](const slang::ast::ParameterSymbol& param)
                -> std::optional<std::string> {
                slang::ConstantValue value = param.getValue();
                if (value.bad())
                {
                    return std::nullopt;
                }
                if (!value.isInteger())
                {
                    value = value.convertToInt();
                }
                if (!value.isInteger())
                {
                    return std::nullopt;
                }
                const slang::SVInt& literal = value.integer();
                if (literal.hasUnknown())
                {
                    return std::nullopt;
                }
                return formatIntegerLiteral(literal);
            };

            ExprNode node;
            node.location = expr.sourceRange.start();

            if (const slang::ConstantValue* constant = expr.getConstant())
            {
                if (constant->isInteger())
                {
                    const slang::SVInt& literal = constant->integer();
                    if (!literal.hasUnknown())
                    {
                        node.kind = ExprNodeKind::Constant;
                        node.literal = formatIntegerLiteral(literal);
                        return addNode(expr, std::move(node));
                    }
                }
            }

            if (const auto* named = expr.as_if<slang::ast::NamedValueExpression>())
            {
                if (const auto* param =
                        named->symbol.as_if<slang::ast::ParameterSymbol>())
                {
                    if (auto literal = paramLiteral(*param))
                    {
                        node.kind = ExprNodeKind::Constant;
                        node.literal = *literal;
                        return addNode(expr, std::move(node));
                    }
                }
                node.kind = ExprNodeKind::Symbol;
                node.symbol = lookupPlanSignal(state_.plan_, named->symbol);
                if (const PortInfo::InoutBinding* inout =
                        state_.resolveInoutBinding(node.symbol))
                {
                    node.symbol = inout->inSymbol;
                }
                if (!node.symbol.valid())
                {
                    reportUnsupported(expr, "Unknown symbol in connection expression");
                }
                return addNode(expr, std::move(node));
            }
            if (const auto* hier = expr.as_if<slang::ast::HierarchicalValueExpression>())
            {
                if (const auto* param =
                        hier->symbol.as_if<slang::ast::ParameterSymbol>())
                {
                    if (auto literal = paramLiteral(*param))
                    {
                        node.kind = ExprNodeKind::Constant;
                        node.literal = *literal;
                        return addNode(expr, std::move(node));
                    }
                }
                auto path = extractXmrPath(*hier);
                if (!path)
                {
                    reportUnsupported(expr, "Unsupported hierarchical symbol in connection");
                    return kInvalidPlanIndex;
                }
                node.kind = ExprNodeKind::XmrRead;
                node.xmrPath = std::move(*path);
                node.isSigned = expr.type ? expr.type->isSigned() : hier->symbol.getType().isSigned();
                if (node.widthHint == 0 && expr.type)
                {
                    uint64_t width = expr.type->getBitstreamWidth();
                    if (width == 0)
                    {
                        if (auto effective = expr.getEffectiveWidth())
                        {
                            width = *effective;
                        }
                    }
                    if (width > 0)
                    {
                        const uint64_t maxValue =
                            static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                        node.widthHint = width > maxValue
                                             ? std::numeric_limits<int32_t>::max()
                                             : static_cast<int32_t>(width);
                    }
                }
                return addNode(expr, std::move(node));
            }
            if (const auto* literal = expr.as_if<slang::ast::IntegerLiteral>())
            {
                node.kind = ExprNodeKind::Constant;
                node.literal = formatIntegerLiteral(literal->getValue());
                node.isSigned = expr.type ? expr.type->isSigned()
                                          : literal->getValue().isSigned();
                return addNode(expr, std::move(node));
            }
            if (const auto* literal = expr.as_if<slang::ast::UnbasedUnsizedIntegerLiteral>())
            {
                node.kind = ExprNodeKind::Constant;
                node.literal = formatIntegerLiteral(literal->getValue());
                node.isSigned = expr.type ? expr.type->isSigned()
                                          : literal->getValue().isSigned();
                return addNode(expr, std::move(node));
            }
            if (const auto* literal = expr.as_if<slang::ast::RealLiteral>())
            {
                node.kind = ExprNodeKind::Constant;
                std::ostringstream oss;
                oss << literal->getValue();
                node.literal = oss.str();
                return addNode(expr, std::move(node));
            }
            if (const auto* literal = expr.as_if<slang::ast::StringLiteral>())
            {
                node.kind = ExprNodeKind::Constant;
                node.literal.assign(literal->getRawValue());
                return addNode(expr, std::move(node));
            }
            if (const auto* conversion = expr.as_if<slang::ast::ConversionExpression>())
            {
                return lowerExpression(conversion->operand());
            }
            if (const auto* unary = expr.as_if<slang::ast::UnaryExpression>())
            {
                if (unary->op == slang::ast::UnaryOperator::Plus ||
                    unary->op == slang::ast::UnaryOperator::Minus)
                {
                    ExprNodeId operand = lowerExpression(unary->operand());
                    if (operand == kInvalidPlanIndex)
                    {
                        return kInvalidPlanIndex;
                    }
                    bool operandSigned = false;
                    if (operand < state_.lowering_.values.size())
                    {
                        operandSigned = state_.lowering_.values[operand].isSigned;
                    }
                    int32_t targetWidth = 0;
                    if (expr.type)
                    {
                        uint64_t width = expr.type->getBitstreamWidth();
                        if (width == 0)
                        {
                            if (auto effective = expr.getEffectiveWidth())
                            {
                                width = *effective;
                            }
                        }
                        if (width > 0)
                        {
                            const uint64_t maxValue =
                                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                            targetWidth = width > maxValue
                                              ? std::numeric_limits<int32_t>::max()
                                              : static_cast<int32_t>(width);
                        }
                    }
                    if (unary->op == slang::ast::UnaryOperator::Plus)
                    {
                        lowered_.emplace(&expr, operand);
                        return operand;
                    }
                    const bool signExtend =
                        expr.type ? expr.type->isSigned() : operandSigned;
                    ExprNode zeroNode;
                    zeroNode.kind = ExprNodeKind::Constant;
                    zeroNode.literal = "0";
                    zeroNode.location = expr.sourceRange.start();
                    zeroNode.isSigned = signExtend;
                    if (targetWidth > 0)
                    {
                        zeroNode.widthHint = targetWidth;
                    }
                    ExprNodeId zeroId = addSyntheticNode(std::move(zeroNode));
                    node.kind = ExprNodeKind::Operation;
                    node.op = wolvrix::lib::grh::OperationKind::kSub;
                    node.operands = {zeroId, operand};
                    node.isSigned = signExtend;
                    if (targetWidth > 0)
                    {
                        node.widthHint = targetWidth;
                    }
                    return addNode(expr, std::move(node));
                }
                const auto opKind = mapUnaryOp(unary->op);
                if (!opKind)
                {
                    reportUnsupported(expr, "Unsupported unary operator");
                    return kInvalidPlanIndex;
                }
                ExprNodeId operand = lowerExpression(unary->operand());
                if (operand == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                node.kind = ExprNodeKind::Operation;
                node.op = *opKind;
                node.operands = {operand};
                return addNode(expr, std::move(node));
            }
            if (const auto* binary = expr.as_if<slang::ast::BinaryExpression>())
            {
                const auto opKind = mapBinaryOp(binary->op);
                if (!opKind)
                {
                    reportUnsupported(expr, "Unsupported binary operator");
                    return kInvalidPlanIndex;
                }
                ExprNodeId lhs = lowerExpression(binary->left());
                ExprNodeId rhs = lowerExpression(binary->right());
                if (lhs == kInvalidPlanIndex || rhs == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                node.kind = ExprNodeKind::Operation;
                node.op = *opKind;
                node.operands = {lhs, rhs};
                return addNode(expr, std::move(node));
            }
            if (const auto* cond = expr.as_if<slang::ast::ConditionalExpression>())
            {
                if (cond->conditions.empty())
                {
                    reportUnsupported(expr, "Conditional expression missing condition");
                    return kInvalidPlanIndex;
                }
                if (cond->conditions.size() > 1)
                {
                    reportUnsupported(expr, "Conditional expression with patterns unsupported");
                }
                const slang::ast::Expression& condExpr = *cond->conditions.front().expr;
                ExprNodeId condId = lowerExpression(condExpr);
                ExprNodeId lhs = lowerExpression(cond->left());
                ExprNodeId rhs = lowerExpression(cond->right());
                if (condId == kInvalidPlanIndex || lhs == kInvalidPlanIndex ||
                    rhs == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                node.kind = ExprNodeKind::Operation;
                node.op = wolvrix::lib::grh::OperationKind::kMux;
                node.operands = {condId, lhs, rhs};
                return addNode(expr, std::move(node));
            }
            if (const auto* concat = expr.as_if<slang::ast::ConcatenationExpression>())
            {
                std::vector<ExprNodeId> operands;
                operands.reserve(concat->operands().size());
                for (const slang::ast::Expression* operand : concat->operands())
                {
                    if (!operand)
                    {
                        continue;
                    }
                    ExprNodeId id = lowerExpression(*operand);
                    if (id == kInvalidPlanIndex)
                    {
                        return kInvalidPlanIndex;
                    }
                    operands.push_back(id);
                }
                node.kind = ExprNodeKind::Operation;
                node.op = wolvrix::lib::grh::OperationKind::kConcat;
                node.operands = std::move(operands);
                return addNode(expr, std::move(node));
            }
            if (const auto* repl = expr.as_if<slang::ast::ReplicationExpression>())
            {
                ExprNodeId count = lowerExpression(repl->count());
                ExprNodeId concat = lowerExpression(repl->concat());
                if (count == kInvalidPlanIndex || concat == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                node.kind = ExprNodeKind::Operation;
                node.op = wolvrix::lib::grh::OperationKind::kReplicate;
                node.operands = {count, concat};
                return addNode(expr, std::move(node));
            }
            if (const auto* select = expr.as_if<slang::ast::ElementSelectExpression>())
            {
                ExprNodeId value = lowerExpression(select->value());
                ExprNodeId index = lowerExpression(select->selector());
                if (value == kInvalidPlanIndex || index == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                if (auto elementWidth = getPackedArrayElementWidth(select->value().type))
                {
                    index = adjustPackedElementLaneIndex(select->value().type, index,
                                                         select->sourceRange.start());
                    node.kind = ExprNodeKind::Operation;
                    node.op = wolvrix::lib::grh::OperationKind::kSliceArray;
                    node.operands = {value, index};
                    node.widthHint = *elementWidth;
                    node.sliceWidth = *elementWidth;
                    return addNode(expr, std::move(node));
                }
                index = adjustPackedIndex(select->value().type, index,
                                          select->sourceRange.start());
                node.kind = ExprNodeKind::Operation;
                node.op = wolvrix::lib::grh::OperationKind::kSliceDynamic;
                node.operands = {value, index};
                return addNode(expr, std::move(node));
            }
            if (const auto* range = expr.as_if<slang::ast::RangeSelectExpression>())
            {
                ExprNodeId value = lowerExpression(range->value());
                ExprNodeId left = lowerExpression(range->left());
                ExprNodeId right = lowerExpression(range->right());
                if (value == kInvalidPlanIndex || left == kInvalidPlanIndex ||
                    right == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                auto getWidthHint = [&](ExprNodeId id) -> int32_t {
                    if (id == kInvalidPlanIndex ||
                        id >= state_.lowering_.values.size())
                    {
                        return 0;
                    }
                    return state_.lowering_.values[id].widthHint;
                };
                int32_t indexWidth = getWidthHint(left);
                if (indexWidth <= 0)
                {
                    indexWidth = getWidthHint(right);
                }
                if (indexWidth <= 0)
                {
                    indexWidth = 32;
                }
                auto addConst = [&](int64_t literal) -> ExprNodeId {
                    ExprNode constNode;
                    constNode.kind = ExprNodeKind::Constant;
                    constNode.literal = std::to_string(literal);
                    constNode.location = range->sourceRange.start();
                    constNode.widthHint = indexWidth;
                    return addSyntheticNode(std::move(constNode));
                };
                auto addSub = [&](ExprNodeId lhs, ExprNodeId rhs) -> ExprNodeId {
                    ExprNode subNode;
                    subNode.kind = ExprNodeKind::Operation;
                    subNode.op = wolvrix::lib::grh::OperationKind::kSub;
                    subNode.operands = {lhs, rhs};
                    subNode.location = range->sourceRange.start();
                    subNode.widthHint = indexWidth;
                    return addSyntheticNode(std::move(subNode));
                };
                auto addAdd = [&](ExprNodeId lhs, ExprNodeId rhs) -> ExprNodeId {
                    ExprNode addNodeValue;
                    addNodeValue.kind = ExprNodeKind::Operation;
                    addNodeValue.op = wolvrix::lib::grh::OperationKind::kAdd;
                    addNodeValue.operands = {lhs, rhs};
                    addNodeValue.location = range->sourceRange.start();
                    addNodeValue.widthHint = indexWidth;
                    return addSyntheticNode(std::move(addNodeValue));
                };
                ExprNodeId indexExpr = kInvalidPlanIndex;
                switch (range->getSelectionKind())
                {
                case slang::ast::RangeSelectionKind::Simple: {
                    auto leftConst = evalConstInt(state_.plan_, state_.lowering_, left);
                    auto rightConst = evalConstInt(state_.plan_, state_.lowering_, right);
                    if (!leftConst || !rightConst)
                    {
                        reportUnsupported(expr, "Dynamic range select is unsupported");
                        return kInvalidPlanIndex;
                    }
                    const int64_t low = std::min(*leftConst, *rightConst);
                    indexExpr = addConst(low);
                    break;
                }
                case slang::ast::RangeSelectionKind::IndexedUp:
                    indexExpr = left;
                    break;
                case slang::ast::RangeSelectionKind::IndexedDown: {
                    auto widthConst = evalConstInt(state_.plan_, state_.lowering_, right);
                    if (widthConst)
                    {
                        if (*widthConst <= 0)
                        {
                            reportUnsupported(expr, "Indexed part-select width must be positive");
                            return kInvalidPlanIndex;
                        }
                        if (*widthConst == 1)
                        {
                            indexExpr = left;
                        }
                        else
                        {
                            ExprNodeId offset = addConst(*widthConst - 1);
                            indexExpr = addSub(left, offset);
                        }
                    }
                    else
                    {
                        ExprNodeId one = addConst(1);
                        ExprNodeId widthMinus = addSub(right, one);
                        indexExpr = addSub(left, widthMinus);
                    }
                    break;
                }
                default:
                    reportUnsupported(expr, "Unsupported range select kind");
                    return kInvalidPlanIndex;
                }
                if (indexExpr == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                if (auto packedElement = getPackedArrayElementSlice(range->value().type))
                {
                    const auto& canonical = range->value().type->getCanonicalType();
                    const auto& packed = canonical.as<slang::ast::PackedArrayType>();
                    const bool ascending = packed.range.left <= packed.range.right;
                    ExprNodeId elementStart = indexExpr;
                    if (range->getSelectionKind() == slang::ast::RangeSelectionKind::Simple)
                    {
                        auto leftConst = evalConstInt(state_.plan_, state_.lowering_, left);
                        auto rightConst = evalConstInt(state_.plan_, state_.lowering_, right);
                        if (!leftConst || !rightConst)
                        {
                            reportUnsupported(expr, "Dynamic range select is unsupported");
                            return kInvalidPlanIndex;
                        }
                        const int64_t low =
                            std::min<int64_t>(packed.range.left, packed.range.right);
                        const int64_t high =
                            std::max<int64_t>(packed.range.left, packed.range.right);
                        auto laneOf = [&](int64_t svIndex) -> int64_t {
                            return ascending ? high - svIndex : svIndex - low;
                        };
                        const int64_t startLane =
                            std::min(laneOf(*leftConst), laneOf(*rightConst));
                        indexExpr = addConst(startLane * packedElement->second);
                    }
                    else
                    {
                        auto widthConst = evalConstInt(state_.plan_, state_.lowering_, right);
                        if (widthConst && *widthConst > 1)
                        {
                            const bool needUpperEndpoint =
                                (range->getSelectionKind() ==
                                     slang::ast::RangeSelectionKind::IndexedUp &&
                                 ascending) ||
                                (range->getSelectionKind() ==
                                     slang::ast::RangeSelectionKind::IndexedDown &&
                                 !ascending);
                            if (needUpperEndpoint)
                            {
                                ExprNodeId offset = addConst(*widthConst - 1);
                                if (range->getSelectionKind() ==
                                    slang::ast::RangeSelectionKind::IndexedUp)
                                {
                                    elementStart = addAdd(left, offset);
                                }
                                else
                                {
                                    elementStart = addSub(left, offset);
                                }
                            }
                        }
                        elementStart =
                            adjustPackedElementLaneIndex(range->value().type,
                                                         elementStart,
                                                         range->sourceRange.start());
                        indexExpr =
                            scalePackedElementLaneIndex(elementStart,
                                                        packedElement->second,
                                                        range->sourceRange.start());
                    }
                }
                else
                {
                    indexExpr =
                        adjustPackedIndex(range->value().type, indexExpr,
                                          range->sourceRange.start());
                }
                if (indexExpr == kInvalidPlanIndex)
                {
                    return kInvalidPlanIndex;
                }
                node.kind = ExprNodeKind::Operation;
                node.op = wolvrix::lib::grh::OperationKind::kSliceDynamic;
                node.operands = {value, indexExpr};
                return addNode(expr, std::move(node));
            }

            reportUnsupported(expr, "Unsupported connection expression");
            return kInvalidPlanIndex;
        }

    private:
        ExprNodeId addNode(const slang::ast::Expression& expr, ExprNode node)
        {
            if (node.widthHint == 0)
            {
                uint64_t width = expr.type->getBitstreamWidth();
                if (width == 0)
                {
                    if (auto effective = expr.getEffectiveWidth())
                    {
                        width = *effective;
                    }
                }
                if (width > 0)
                {
                    const uint64_t maxValue =
                        static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
                    node.widthHint = width > maxValue
                                         ? std::numeric_limits<int32_t>::max()
                                         : static_cast<int32_t>(width);
                }
            }
            if (expr.type)
            {
                node.valueType = classifyValueType(*expr.type);
            }
            const ExprNodeId id =
                static_cast<ExprNodeId>(state_.lowering_.values.size());
            state_.lowering_.values.push_back(std::move(node));
            state_.registerExprNode();
            lowered_.emplace(&expr, id);
            return id;
        }

        ExprNodeId addSyntheticNode(ExprNode node)
        {
            const ExprNodeId id =
                static_cast<ExprNodeId>(state_.lowering_.values.size());
            state_.lowering_.values.push_back(std::move(node));
            state_.registerExprNode();
            return id;
        }

        ExprNodeId makeSliceDynamic(ExprNodeId value, ExprNodeId index, int32_t width,
                                    slang::SourceLocation location)
        {
            if (value == kInvalidPlanIndex || index == kInvalidPlanIndex)
            {
                return kInvalidPlanIndex;
            }
            ExprNode node;
            node.kind = ExprNodeKind::Operation;
            node.op = wolvrix::lib::grh::OperationKind::kSliceDynamic;
            node.operands = {value, index};
            node.location = location;
            node.widthHint = width > 0 ? width : 1;
            return addSyntheticNode(std::move(node));
        }

        ExprNodeId resizeValueToWidth(ExprNodeId value, int32_t targetWidth,
                                      bool signExtend, slang::SourceLocation location)
        {
            if (value == kInvalidPlanIndex || targetWidth <= 0)
            {
                return value;
            }
            int32_t sourceWidth = 0;
            if (value < state_.lowering_.values.size())
            {
                sourceWidth = state_.lowering_.values[value].widthHint;
            }
            if (sourceWidth <= 0 || sourceWidth == targetWidth)
            {
                return value;
            }
            if (targetWidth < sourceWidth)
            {
                ExprNode zeroNode;
                zeroNode.kind = ExprNodeKind::Constant;
                zeroNode.literal = "0";
                zeroNode.location = location;
                zeroNode.widthHint = targetWidth;
                ExprNodeId zeroId = addSyntheticNode(std::move(zeroNode));
                return makeSliceDynamic(value, zeroId, targetWidth, location);
            }

            const int32_t padWidth = targetWidth - sourceWidth;
            ExprNodeId pad = kInvalidPlanIndex;
            if (signExtend)
            {
                ExprNode signIndexNode;
                signIndexNode.kind = ExprNodeKind::Constant;
                signIndexNode.literal = std::to_string(sourceWidth - 1);
                signIndexNode.location = location;
                signIndexNode.widthHint = targetWidth;
                ExprNodeId signIndex = addSyntheticNode(std::move(signIndexNode));
                ExprNodeId signBit = makeSliceDynamic(value, signIndex, 1, location);
                ExprNode countNode;
                countNode.kind = ExprNodeKind::Constant;
                countNode.literal = std::to_string(padWidth);
                countNode.location = location;
                countNode.widthHint = 32;
                ExprNodeId countId = addSyntheticNode(std::move(countNode));
                ExprNode repNode;
                repNode.kind = ExprNodeKind::Operation;
                repNode.op = wolvrix::lib::grh::OperationKind::kReplicate;
                repNode.operands = {countId, signBit};
                repNode.location = location;
                repNode.widthHint = padWidth;
                pad = addSyntheticNode(std::move(repNode));
            }
            else
            {
                ExprNode padNode;
                padNode.kind = ExprNodeKind::Constant;
                padNode.literal = std::to_string(padWidth) + "'b0";
                padNode.location = location;
                padNode.widthHint = padWidth;
                pad = addSyntheticNode(std::move(padNode));
            }
            if (pad == kInvalidPlanIndex)
            {
                return value;
            }
            ExprNode concatNode;
            concatNode.kind = ExprNodeKind::Operation;
            concatNode.op = wolvrix::lib::grh::OperationKind::kConcat;
            concatNode.operands = {pad, value};
            concatNode.location = location;
            concatNode.widthHint = targetWidth;
            return addSyntheticNode(std::move(concatNode));
        }

        void reportUnsupported(const slang::ast::Expression& expr, std::string_view message)
        {
            if (!state_.context_.diagnostics)
            {
                return;
            }
            state_.context_.diagnostics->error(expr.sourceRange.start(), std::string(message));
        }

        GraphAssemblyState& state_;
        std::unordered_map<const slang::ast::Expression*, ExprNodeId> lowered_;
    };

    wolvrix::lib::grh::ValueId emitConstant(const ExprNode& node)
    {
        wolvrix::lib::grh::SymbolId symbol = makeConstValueSymbol();
        int32_t width = node.widthHint;
        if (width <= 0)
        {
            try
            {
                slang::SVInt parsed = slang::SVInt::fromString(node.literal);
                if (!parsed.hasUnknown())
                {
                    width = static_cast<int32_t>(parsed.getBitWidth());
                }
            }
            catch (const std::exception&)
            {
                width = 0;
            }
        }
        width = normalizeWidth(width);
        wolvrix::lib::grh::ValueId value =
            graph_.createValue(symbol, width, node.isSigned, node.valueType);
        wolvrix::lib::grh::OperationId op =
            createOp(wolvrix::lib::grh::OperationKind::kConstant,
                     wolvrix::lib::grh::SymbolId::invalid(),
                     node.location);
        graph_.addResult(op, value);
        graph_.setAttr(op, "constValue", node.literal);
        return value;
    }

    wolvrix::lib::grh::ValueId emitExpr(ExprNodeId id)
    {
        if (id == kInvalidPlanIndex || id >= lowering_.values.size())
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }
        if (valueByExpr_[id].valid())
        {
            return valueByExpr_[id];
        }
        if (memoryReadIndexByExpr_[id] != kInvalidMemoryReadIndex)
        {
            wolvrix::lib::grh::ValueId value = emitMemoryRead(id);
            if (value.valid())
            {
                return value;
            }
        }
        const ExprNode& node = lowering_.values[id];
        if (node.kind == ExprNodeKind::Constant)
        {
            wolvrix::lib::grh::ValueId value = emitConstant(node);
            valueByExpr_[id] = value;
            return value;
        }
        if (node.kind == ExprNodeKind::Symbol)
        {
            wolvrix::lib::grh::ValueId value =
                isMemoryBackedPackedAggregate(node.symbol)
                    ? emitPackedAggregateWholeRead(node.symbol, node.location)
                    : valueForSymbol(node.symbol);
            if (!value.valid() && context_.diagnostics)
            {
                context_.diagnostics->error(node.location,
                                            "Graph assembly missing symbol value");
            }
            valueByExpr_[id] = value;
            return value;
        }
        if (node.kind == ExprNodeKind::XmrRead)
        {
            const int32_t width = normalizeWidth(node.widthHint);
            wolvrix::lib::grh::SymbolId sym = makeInternalValueSymbol();
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, width, node.isSigned, node.valueType);
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kXMRRead,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         node.location);
            graph_.addResult(op, result);
            graph_.setAttr(op, "xmrPath", node.xmrPath);
            valueByExpr_[id] = result;
            return result;
        }
        if (node.kind != ExprNodeKind::Operation)
        {
            return wolvrix::lib::grh::ValueId::invalid();
        }

        std::vector<wolvrix::lib::grh::ValueId> operands;
        operands.reserve(node.operands.size());
        for (ExprNodeId operandId : node.operands)
        {
            wolvrix::lib::grh::ValueId operandValue = emitExpr(operandId);
            if (!operandValue.valid())
            {
                return wolvrix::lib::grh::ValueId::invalid();
            }
            operands.push_back(operandValue);
        }

        if (node.op == wolvrix::lib::grh::OperationKind::kReplicate && operands.size() >= 2)
        {
            auto count = evalConstInt(plan_, lowering_, node.operands[0]);
            if (!count || *count <= 0)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        node.location,
                        "Replication count must be constant in graph assembly");
                }
                return wolvrix::lib::grh::ValueId::invalid();
            }
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kReplicate,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         node.location);
            graph_.addOperand(op, operands[1]);
            graph_.setAttr(op, "rep", static_cast<int64_t>(*count));
            wolvrix::lib::grh::SymbolId sym =
                makeInternalValueSymbol();
            const int32_t width = normalizeWidth(node.widthHint);
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, width, false, node.valueType);
            graph_.addResult(op, result);
            valueByExpr_[id] = result;
            return result;
        }

        if (node.op == wolvrix::lib::grh::OperationKind::kSliceDynamic && operands.size() >= 2)
        {
            const int32_t width = normalizeWidth(node.widthHint);
            std::optional<int64_t> staticOffset =
                evalConstInt(plan_, lowering_, node.operands[1]);
            if (width > 0)
            {
                const wolvrix::lib::grh::Value offsetValue = graph_.getValue(operands[1]);
                const wolvrix::lib::grh::OperationId defOpId = offsetValue.definingOp();
                if (!staticOffset && defOpId.valid())
                {
                    const wolvrix::lib::grh::Operation defOp = graph_.getOperation(defOpId);
                    if (defOp.kind() == wolvrix::lib::grh::OperationKind::kConstant)
                    {
                        if (auto literal = defOp.attr("constValue"))
                        {
                            if (auto text = std::get_if<std::string>(&*literal))
                            {
                                try
                                {
                                    slang::SVInt parsed = slang::SVInt::fromString(*text);
                                    if (!parsed.hasUnknown())
                                    {
                                        if (auto offset = parsed.as<int64_t>())
                                        {
                                            staticOffset = *offset;
                                        }
                                    }
                                }
                                catch (const std::exception&)
                                {
                                    staticOffset.reset();
                                }
                            }
                        }
                    }
                }
            }

            if (staticOffset && *staticOffset >= 0)
            {
                const int64_t baseWidth = graph_.getValue(operands[0]).width();
                const int64_t start = *staticOffset;
                const int64_t end = start + static_cast<int64_t>(width) - 1;
                if (baseWidth <= 0 || (end >= start && end < baseWidth))
                {
                    wolvrix::lib::grh::ValueId result =
                        createStaticSliceValue(operands[0], start, end,
                                               false, node.valueType, node.location);
                    if (result.valid())
                    {
                        valueByExpr_[id] = result;
                        return result;
                    }
                }
            }

            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kSliceDynamic,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         node.location);
            graph_.addOperand(op, operands[0]);
            graph_.addOperand(op, operands[1]);
            graph_.setAttr(op, "sliceWidth", static_cast<int64_t>(width));
            wolvrix::lib::grh::SymbolId sym =
                makeInternalValueSymbol();
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, width, false, node.valueType);
            graph_.addResult(op, result);
            valueByExpr_[id] = result;
            return result;
        }

        if (node.op == wolvrix::lib::grh::OperationKind::kSliceArray && operands.size() >= 2)
        {
            const int32_t width = normalizeWidth(node.sliceWidth > 0 ? node.sliceWidth
                                                                     : node.widthHint);
            std::optional<int64_t> staticIndex =
                evalConstInt(plan_, lowering_, node.operands[1]);
            if (staticIndex && *staticIndex >= 0)
            {
                const int64_t baseWidth = graph_.getValue(operands[0]).width();
                const int64_t start = *staticIndex * static_cast<int64_t>(width);
                const int64_t end = start + static_cast<int64_t>(width) - 1;
                if (baseWidth <= 0 || (end >= start && end < baseWidth))
                {
                    wolvrix::lib::grh::ValueId result =
                        createStaticSliceValue(operands[0], start, end,
                                               false, node.valueType, node.location);
                    if (result.valid())
                    {
                        valueByExpr_[id] = result;
                        return result;
                    }
                }
            }

            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kSliceArray,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         node.location);
            graph_.addOperand(op, operands[0]);
            graph_.addOperand(op, operands[1]);
            graph_.setAttr(op, "sliceWidth", static_cast<int64_t>(width));
            wolvrix::lib::grh::SymbolId sym =
                makeInternalValueSymbol();
            wolvrix::lib::grh::ValueId result =
                graph_.createValue(sym, width, false, node.valueType);
            graph_.addResult(op, result);
            valueByExpr_[id] = result;
            return result;
        }

        wolvrix::lib::grh::OperationId op =
            createOp(node.op, wolvrix::lib::grh::SymbolId::invalid(), node.location);
        for (const auto& operand : operands)
        {
            graph_.addOperand(op, operand);
        }
        if (node.op == wolvrix::lib::grh::OperationKind::kSystemFunction)
        {
            if (!node.systemName.empty())
            {
                graph_.setAttr(op, "name", node.systemName);
            }
            if (node.hasSideEffects)
            {
                graph_.setAttr(op, "hasSideEffects", true);
            }
            graph_.setAttr(op, "procKind", procKindText(node.procKind));
            graph_.setAttr(op, "hasTiming", node.hasTiming);
        }

        wolvrix::lib::grh::SymbolId sym =
            makeInternalValueSymbol();
        int32_t width = node.widthHint;
        if (width <= 0)
        {
            switch (node.op)
            {
            case wolvrix::lib::grh::OperationKind::kLogicAnd:
            case wolvrix::lib::grh::OperationKind::kLogicOr:
            case wolvrix::lib::grh::OperationKind::kLogicNot:
            case wolvrix::lib::grh::OperationKind::kEq:
            case wolvrix::lib::grh::OperationKind::kNe:
            case wolvrix::lib::grh::OperationKind::kCaseEq:
            case wolvrix::lib::grh::OperationKind::kCaseNe:
            case wolvrix::lib::grh::OperationKind::kWildcardEq:
            case wolvrix::lib::grh::OperationKind::kWildcardNe:
            case wolvrix::lib::grh::OperationKind::kLt:
            case wolvrix::lib::grh::OperationKind::kLe:
            case wolvrix::lib::grh::OperationKind::kGt:
            case wolvrix::lib::grh::OperationKind::kGe:
                width = 1;
                break;
            case wolvrix::lib::grh::OperationKind::kMux:
                if (operands.size() > 1)
                {
                    width = graph_.getValue(operands[1]).width();
                }
                break;
            case wolvrix::lib::grh::OperationKind::kConcat:
            {
                int32_t sum = 0;
                for (const auto& operand : operands)
                {
                    sum += graph_.getValue(operand).width();
                }
                width = sum;
                break;
            }
            default:
                if (!operands.empty())
                {
                    width = graph_.getValue(operands.front()).width();
                }
                break;
            }
        }
        width = normalizeWidth(width);
        wolvrix::lib::grh::ValueId result =
            graph_.createValue(sym, width, node.isSigned, node.valueType);
        graph_.addResult(op, result);
        valueByExpr_[id] = result;
        return result;
    }

    static std::string edgeText(EventEdge edge)
    {
        switch (edge)
        {
        case EventEdge::Posedge:
            return "posedge";
        case EventEdge::Negedge:
            return "negedge";
        default:
            return "posedge";
        }
    }

    void emitWriteBack()
    {
        auto makeMaskLiteral = [&](int32_t width, int64_t low, int64_t span) -> std::string {
            const int32_t normalized = normalizeWidth(width);
            std::string bits(static_cast<std::size_t>(normalized), '0');
            if (span > 0)
            {
                const int64_t end = low + span;
                for (int64_t bit = low; bit < end; ++bit)
                {
                    if (bit < 0 || bit >= normalized)
                    {
                        continue;
                    }
                    const std::size_t index =
                        static_cast<std::size_t>(normalized - 1 - bit);
                    bits[index] = '1';
                }
            }
            return std::to_string(normalized) + "'b" + bits;
        };
        auto makeMaskValue = [&](int32_t width, int64_t low, int64_t span,
                                 slang::SourceLocation location) -> wolvrix::lib::grh::ValueId {
            wolvrix::lib::grh::SymbolId sym = makeInternalValueSymbol();
            const int32_t normalized = normalizeWidth(width);
            wolvrix::lib::grh::ValueId value =
                graph_.createValue(sym, normalized, false, wolvrix::lib::grh::ValueType::Logic);
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kConstant,
                         wolvrix::lib::grh::SymbolId::invalid(),
                         location);
            graph_.addResult(op, value);
            graph_.setAttr(op, "constValue", makeMaskLiteral(normalized, low, span));
            return value;
        };

        for (const auto& entry : writeBack_.entries)
        {
            if (!entry.target.valid())
            {
                continue;
            }
            if (instanceMergedTargets_.find(entry.target.index) != instanceMergedTargets_.end())
            {
                continue;
            }

            if (entry.domain == ControlDomain::Combinational)
            {
                wolvrix::lib::grh::ValueId targetValue = valueForSymbol(entry.target);
                if (!targetValue.valid())
                {
                    if (context_.diagnostics)
                    {
                        context_.diagnostics->warn(entry.location,
                                                   "Write-back target missing value");
                    }
                    continue;
                }
                wolvrix::lib::grh::ValueId nextValue = emitExpr(entry.nextValue);
                if (!nextValue.valid())
                {
                    continue;
                }
                annotatePackedArrayConcatDefop(entry.target, nextValue);
                wolvrix::lib::grh::OperationId op =
                    createOp(wolvrix::lib::grh::OperationKind::kAssign,
                             wolvrix::lib::grh::SymbolId::invalid(),
                             entry.location);
                graph_.addOperand(op, nextValue);
                graph_.addResult(op, targetValue);
                continue;
            }

            const bool isSequential = entry.domain == ControlDomain::Sequential;
            const bool isLatch = entry.domain == ControlDomain::Latch;
            if (!isSequential && !isLatch)
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(entry.location,
                                               "Skipping unsupported control domain");
                }
                continue;
            }

            wolvrix::lib::grh::ValueId updateCond = emitExpr(entry.updateCond);
            wolvrix::lib::grh::ValueId nextValue = emitExpr(entry.nextValue);
            if (!updateCond.valid() || !nextValue.valid())
            {
                continue;
            }
            if (isSequential && entry.eventEdges.empty())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(
                        entry.location,
                        "Skipping register write without edge-sensitive timing control");
                }
                continue;
            }

            wolvrix::lib::grh::ValueId targetValue = valueForSymbol(entry.target);
            if (!targetValue.valid())
            {
                if (context_.diagnostics)
                {
                    context_.diagnostics->warn(entry.location,
                                               "Write-back target missing value");
                }
                continue;
            }
            const int32_t targetWidth = graph_.getValue(targetValue).width();
            int64_t maskLow = 0;
            int64_t maskSpan = targetWidth;
            if (entry.hasStaticSlice && entry.sliceWidth > 0 &&
                entry.sliceLow >= 0 &&
                entry.sliceLow + entry.sliceWidth <= targetWidth)
            {
                maskLow = entry.sliceLow;
                maskSpan = entry.sliceWidth;
            }
            wolvrix::lib::grh::ValueId maskValue =
                makeMaskValue(targetWidth, maskLow, maskSpan, entry.location);
            if (!maskValue.valid())
            {
                continue;
            }

            if (isSequential)
            {
                std::string_view regName = plan_.symbolTable.text(entry.target);
                if (regName.empty())
                {
                    continue;
                }
                wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol();
                wolvrix::lib::grh::OperationId op =
                    createOp(wolvrix::lib::grh::OperationKind::kRegisterWritePort,
                             opSym, entry.location);
                graph_.addOperand(op, updateCond);
                graph_.addOperand(op, nextValue);
                graph_.addOperand(op, maskValue);
                for (ExprNodeId evtId : entry.eventOperands)
                {
                    wolvrix::lib::grh::ValueId evt = emitExpr(evtId);
                    if (!evt.valid())
                    {
                        continue;
                    }
                    graph_.addOperand(op, evt);
                }
                std::vector<std::string> edges;
                edges.reserve(entry.eventEdges.size());
                for (EventEdge edge : entry.eventEdges)
                {
                    edges.push_back(edgeText(edge));
                }
                graph_.setAttr(op, "eventEdge", std::move(edges));
                graph_.setAttr(op, "regSymbol", std::string(regName));
                continue;
            }

            std::string_view latchName = plan_.symbolTable.text(entry.target);
            if (latchName.empty())
            {
                continue;
            }
            wolvrix::lib::grh::SymbolId opSym = makeInternalOpSymbol();
            wolvrix::lib::grh::OperationId op =
                createOp(wolvrix::lib::grh::OperationKind::kLatchWritePort,
                         opSym, entry.location);
            graph_.addOperand(op, updateCond);
            graph_.addOperand(op, nextValue);
            graph_.addOperand(op, maskValue);
            graph_.setAttr(op, "latchSymbol", std::string(latchName));
        }
    }

    ConvertContext& context_;
    GraphAssembler& assembler_;
    wolvrix::lib::grh::Graph& graph_;
    const ModulePlan& plan_;
    LoweringPlan& lowering_;
    const WriteBackPlan& writeBack_;
    const slang::SourceManager* sourceManager_ = nullptr;
    std::vector<wolvrix::lib::grh::SymbolId> symbolIds_;
    std::vector<wolvrix::lib::grh::ValueId> valueBySymbol_;
    std::vector<wolvrix::lib::grh::ValueId> valueByExpr_;
    std::vector<wolvrix::lib::grh::ValueId> packedAggregateWholeReadBySymbol_;
    std::vector<wolvrix::lib::grh::OperationId> memoryOpBySymbol_;
    std::vector<std::string> memorySymbolName_;
    std::vector<StorageKind> storageBySymbol_;
    std::vector<wolvrix::lib::grh::OperationId> storageOpBySymbol_;
    std::vector<slang::SourceLocation> storageLocation_;
    std::vector<int32_t> memoryReadIndexByExpr_;
    ConnectionExprLowerer connectionLowerer_;
    struct EmittedMemoryRead {
        PlanSymbolId memory;
        SignalId signal = kInvalidPlanIndex;
        ExprNodeId address = kInvalidPlanIndex;
        bool isSync = false;
        ExprNodeId updateCond = kInvalidPlanIndex;
        std::vector<EventEdge> eventEdges;
        std::vector<ExprNodeId> eventOperands;
        ExprNodeId expr = kInvalidPlanIndex;
        wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
    };
    std::vector<EmittedMemoryRead> emittedMemoryReads_;
    struct InstanceXmrWrite {
        std::string xmrPath;
        wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
        slang::SourceLocation location{};
    };
    struct InstanceSliceWrite {
        PlanSymbolId target;
        int64_t low = 0;
        int64_t width = 0;
        wolvrix::lib::grh::ValueId value = wolvrix::lib::grh::ValueId::invalid();
        slang::SourceLocation location{};
        bool explicitSlice = false;
    };
    std::vector<InstanceXmrWrite> instanceXmrWrites_;
    std::unordered_map<uint32_t, std::vector<InstanceSliceWrite>> instanceSliceWrites_;
    std::unordered_set<uint32_t> instanceMergedTargets_;
    wolvrix::lib::grh::SymbolId makeInternalOpSymbol()
    {
        return graph_.makeInternalOpSym();
    }

    wolvrix::lib::grh::SymbolId makeInternalValueSymbol()
    {
        return graph_.makeInternalValSym();
    }

    wolvrix::lib::grh::SymbolId makeConstValueSymbol()
    {
        return graph_.makeInternalValSym();
    }
};

namespace {

std::string normalizeGraphIdent(std::string_view text, std::string_view fallback)
{
    std::string out = wolvrix::lib::grh::Graph::normalizeComponent(text);
    if (out.empty())
    {
        out = std::string(fallback);
    }
    unsigned char first = static_cast<unsigned char>(out.front());
    if (!(std::isalpha(first) || first == '_'))
    {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string formatShortHash(std::string_view signature)
{
    const uint64_t hash = std::hash<std::string_view>{}(signature);
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setw(8) << std::setfill('0')
        << static_cast<uint32_t>(hash);
    return oss.str();
}

std::string buildReadableParamSuffix(std::string_view signature, std::string_view base)
{
    if (signature.empty())
    {
        return {};
    }
    std::string suffix = "__p_";
    bool hasPart = false;
    std::size_t start = 0;
    while (start <= signature.size())
    {
        const std::size_t end = signature.find(';', start);
        const std::size_t span =
            (end == std::string_view::npos ? signature.size() : end) - start;
        std::string_view entry = signature.substr(start, span);
        if (!entry.empty())
        {
            const std::size_t eq = entry.find('=');
            std::string_view name = eq == std::string_view::npos ? entry : entry.substr(0, eq);
            std::string_view value = eq == std::string_view::npos ? std::string_view{}
                                                                  : entry.substr(eq + 1);
            std::string nameNorm = normalizeGraphIdent(name, "param");
            std::string valueNorm = normalizeGraphIdent(value, "val");
            if (hasPart)
            {
                suffix.append("__");
            }
            suffix.append(nameNorm);
            suffix.push_back('_');
            suffix.append(valueNorm);
            hasPart = true;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }
    if (!hasPart)
    {
        return {};
    }

    constexpr std::size_t kMaxGraphName = 120;
    if (base.size() + suffix.size() <= kMaxGraphName)
    {
        return suffix;
    }

    const std::string hashTag = "__h" + formatShortHash(signature);
    const std::size_t maxSuffix =
        base.size() < kMaxGraphName ? kMaxGraphName - base.size() : 0;
    if (maxSuffix <= hashTag.size() + 3)
    {
        return "__p" + hashTag;
    }

    const std::size_t keep = maxSuffix - hashTag.size();
    if (suffix.size() > keep)
    {
        suffix.resize(keep);
    }
    while (!suffix.empty() && suffix.back() == '_')
    {
        suffix.pop_back();
    }
    if (suffix.empty())
    {
        suffix = "__p";
    }
    return suffix + hashTag;
}

} // namespace

const std::string& GraphAssembler::resolveGraphName(const PlanKey& key,
                                                    std::string_view moduleName)
{
    std::lock_guard<std::mutex> lock(nameMutex_);
    auto it = graphNames_.find(key);
    if (it != graphNames_.end())
    {
        return it->second;
    }

    std::string base = moduleName.empty() ? std::string("convert_graph") : std::string(moduleName);
    std::string candidate = base;
    if (!key.paramSignature.empty())
    {
        const std::string suffix =
            buildReadableParamSuffix(key.paramSignature, base);
        candidate = suffix.empty() ? base : base + suffix;
    }
    std::string finalName = candidate;
    std::size_t suffix = 0;
    while (reservedGraphNames_.find(finalName) != reservedGraphNames_.end())
    {
        finalName = candidate + "_" + std::to_string(++suffix);
    }

    auto [inserted, added] = graphNames_.emplace(key, std::move(finalName));
    reservedGraphNames_.insert(inserted->second);
    return inserted->second;
}

wolvrix::lib::grh::Graph& GraphAssembler::build(const PlanKey& key, const ModulePlan& plan,
                                      LoweringPlan& lowering, const WriteBackPlan& writeBack)
{
    std::string moduleName;
    if (plan.moduleSymbol.valid())
    {
        moduleName = std::string(plan.symbolTable.text(plan.moduleSymbol));
    }
    const std::string& finalSymbol = resolveGraphName(key, moduleName);
    wolvrix::lib::grh::Graph* graph = nullptr;
    if (designMutex_)
    {
        std::lock_guard<std::mutex> lock(*designMutex_);
        graph = &design_.createGraph(std::string(finalSymbol));
        design_.addDeclaredSymbol(design_.internSymbol(finalSymbol));
    }
    else
    {
        graph = &design_.createGraph(std::string(finalSymbol));
        design_.addDeclaredSymbol(design_.internSymbol(finalSymbol));
    }
    GraphAssemblyState state(context_, *this, *graph, plan, lowering, writeBack);
    state.build();
    return *graph;
}

namespace {

struct ConvertParallelState {
    std::atomic<std::size_t> pending{0};
    std::atomic<bool> cancel{false};
    std::condition_variable doneCv;
    std::mutex doneMutex;
};

struct AbortState {
    std::atomic<bool>& cancel;
    PlanTaskQueue& queue;
    std::atomic<std::size_t>& pending;
    std::condition_variable& doneCv;

    void request()
    {
        bool expected = false;
        if (!cancel.compare_exchange_strong(expected, true))
        {
            return;
        }
        const std::size_t dropped = queue.drain();
        if (dropped > 0)
        {
            const std::size_t prev = pending.fetch_sub(dropped, std::memory_order_relaxed);
            if (prev <= dropped)
            {
                pending.store(0, std::memory_order_relaxed);
            }
        }
        queue.close();
        doneCv.notify_all();
    }
};

struct TopGraphInfo {
    std::vector<PlanKey> order;
    std::unordered_map<PlanKey, std::vector<std::string>, PlanKeyHash> aliases;
    std::unordered_map<PlanKey, std::string, PlanKeyHash> moduleNames;
};

bool shouldCancel(const ConvertContext& context)
{
    return context.cancelFlag && context.cancelFlag->load(std::memory_order_relaxed);
}

void markInstanceReady(ConvertContext& context, const PlanKey& key)
{
    if (context.instanceRegistry)
    {
        context.instanceRegistry->markReady(key);
    }
}

void runSlangPrebind(const slang::ast::RootSymbol& root, Logger* logger, bool timingEnabled)
{
    logPassStart(logger, timingEnabled, "pass0-slang-prebind", {});
    const auto prebindStart = ConvertClock::now();
    // Trigger slang's internal DiagnosticVisitor through the public API.
    // This traverses the entire AST and triggers all lazy bindings,
    // making it safe for multithreaded access afterward.
    (void)root.getCompilation().getSemanticDiagnostics();
    
    // Additional prebind pass: force all expression types to be resolved.
    // getSemanticDiagnostics() uses DiagnosticVisitor with VisitStatements=false
    // and VisitExpressions=false, which may leave some expression types unresolved.
    // This pass explicitly traverses all expressions to ensure thread-safe access.
    ExpressionPrebindVisitor prebindVisitor;
    root.visit(prebindVisitor);
    
    // Do not freeze the compilation here: Convert still evaluates constants
    // (parameters, loop bounds, etc.) during passes and slang allocates constants
    // lazily. Freezing would assert in allocConstant.
    
    const auto prebindEnd = ConvertClock::now();
    logPassTiming(logger, timingEnabled, "pass0-slang-prebind", {}, prebindEnd - prebindStart);
}

void configureParallelContext(ConvertContext& context, ConvertDiagnostics& diagnostics,
                              bool useParallel, ConvertParallelState& state)
{
    if (useParallel)
    {
        context.taskCounter = &state.pending;
        context.cancelFlag = &state.cancel;
        diagnostics.enableThreadLocal(true);
    }
    else
    {
        context.taskCounter = nullptr;
        context.cancelFlag = nullptr;
        diagnostics.enableThreadLocal(false);
    }
}

std::unique_ptr<AbortState> configureAbortHandler(ConvertDiagnostics& diagnostics,
                                                  bool useParallel, bool abortOnError,
                                                  PlanTaskQueue& queue,
                                                  ConvertParallelState& state)
{
    if (useParallel && abortOnError)
    {
        auto abortState = std::make_unique<AbortState>(AbortState{
            state.cancel,
            queue,
            state.pending,
            state.doneCv});
        diagnostics.setOnError([statePtr = abortState.get()]() {
            if (statePtr)
            {
                statePtr->request();
            }
        });
        return abortState;
    }

    if (!useParallel && abortOnError)
    {
        diagnostics.setOnError([]() { throw ConvertAbort(); });
        return nullptr;
    }

    diagnostics.setOnError(nullptr);
    return nullptr;
}

TopGraphInfo collectTopInstances(const slang::ast::RootSymbol& root, ConvertContext& context)
{
    TopGraphInfo info;
    std::unordered_set<PlanKey, PlanKeyHash> seen;

    for (const slang::ast::InstanceSymbol* topInstance : root.topInstances)
    {
        if (!topInstance)
        {
            continue;
        }
        ParameterSnapshot params = snapshotParameters(topInstance->body, nullptr);
        PlanKey topKey;
        topKey.definition = &topInstance->body.getDefinition();
        topKey.body = &topInstance->body;
        topKey.paramSignature = params.signature;
        if (seen.insert(topKey).second)
        {
            info.order.push_back(topKey);
        }
        std::vector<std::string>& aliases = info.aliases[topKey];
        if (!topInstance->name.empty())
        {
            aliases.emplace_back(topInstance->name);
        }
        if (!topInstance->getDefinition().name.empty())
        {
            aliases.emplace_back(topInstance->getDefinition().name);
        }
        if (!topInstance->body.getDefinition().name.empty())
        {
            info.moduleNames.emplace(topKey,
                                     std::string(topInstance->body.getDefinition().name));
        }
        else if (!topInstance->name.empty())
        {
            info.moduleNames.emplace(topKey, std::string(topInstance->name));
        }
        enqueuePlanKey(context, topInstance->body, std::move(params.signature));
    }

    return info;
}

void processPlanKey(PlanKey key, ConvertContext& context, PlanCache& planCache,
                    GraphAssembler& graphAssembler)
{
    if (!key.body)
    {
        markInstanceReady(context, key);
        return;
    }
    if (!key.definition)
    {
        key.definition = &key.body->getDefinition();
    }
    if (shouldCancel(context))
    {
        markInstanceReady(context, key);
        return;
    }
    if (!planCache.tryClaim(key))
    {
        markInstanceReady(context, key);
        return;
    }

    ModulePlanner planner(context);
    StmtLowererPass stmtLowerer(context);
    WriteBackPass writeBack(context);
    MemoryPortLowererPass memoryPortLowerer(context);

    std::string_view moduleName;
    if (key.definition && !key.definition->name.empty())
    {
        moduleName = key.definition->name;
    }
    logPassStart(context.logger, context.options.enableTiming, "pass1-module-plan", moduleName);
    const auto planStart = ConvertClock::now();
    ModulePlan plan = planner.plan(*key.body);
    const auto planEnd = ConvertClock::now();
    if (plan.moduleSymbol.valid())
    {
        moduleName = plan.symbolTable.text(plan.moduleSymbol);
    }
    logPassTiming(context.logger, context.options.enableTiming,
                  "pass1-module-plan", moduleName, planEnd - planStart);

    if (shouldCancel(context))
    {
        markInstanceReady(context, key);
        return;
    }

    LoweringPlan lowering;
    logPassStart(context.logger, context.options.enableTiming,
                 "pass2-stmt-lowerer", moduleName);
    const auto stmtStart = ConvertClock::now();
    stmtLowerer.lower(plan, lowering);
    const auto stmtEnd = ConvertClock::now();
    logPassTiming(context.logger, context.options.enableTiming,
                  "pass2-stmt-lowerer", moduleName, stmtEnd - stmtStart);

    logPassStart(context.logger, context.options.enableTiming,
                 "pass3-writeback", moduleName);
    const auto writeBackStart = ConvertClock::now();
    WriteBackPlan writeBackPlan = writeBack.lower(plan, lowering);
    const auto writeBackEnd = ConvertClock::now();
    logPassTiming(context.logger, context.options.enableTiming,
                  "pass3-writeback", moduleName,
                  writeBackEnd - writeBackStart);

    logPassStart(context.logger, context.options.enableTiming,
                 "pass4-assembly", moduleName);
    const auto assemblyStart = ConvertClock::now();
    memoryPortLowerer.lower(plan, lowering);
    if (shouldCancel(context))
    {
        markInstanceReady(context, key);
        return;
    }
    graphAssembler.build(key, plan, lowering, writeBackPlan);
    const auto assemblyEnd = ConvertClock::now();
    logPassTiming(context.logger, context.options.enableTiming,
                  "pass4-assembly", moduleName,
                  assemblyEnd - assemblyStart);

    planCache.setLoweringPlan(key, std::move(lowering));
    planCache.setWriteBackPlan(key, std::move(writeBackPlan));
    planCache.storePlan(key, std::move(plan));
    markInstanceReady(context, key);
}

template <typename Processor>
void runPlanQueueSerial(PlanTaskQueue& queue, ConvertDiagnostics* diagnostics,
                        Processor&& processKey)
{
    PlanKey key;
    while (queue.tryPop(key))
    {
        processKey(std::move(key));
        if (diagnostics)
        {
            diagnostics->flushThreadLocal();
        }
    }
    if (diagnostics)
    {
        diagnostics->flushThreadLocal();
    }
}

template <typename Processor>
void runPlanQueueParallel(PlanTaskQueue& queue, std::size_t threadCount,
                          ConvertParallelState& state, ConvertDiagnostics* diagnostics,
                          AbortState* abortState, Processor&& processKey,
                          bool abortOnError)
{
    threadCount = std::max<std::size_t>(1, threadCount);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    auto finishTask = [&]() {
        const std::size_t prev = state.pending.fetch_sub(1, std::memory_order_relaxed);
        if (prev <= 1)
        {
            state.pending.store(0, std::memory_order_relaxed);
            queue.close();
            state.doneCv.notify_all();
        }
    };

    auto worker = [&]() {
        PlanKey key;
        while (queue.waitPop(key, &state.cancel))
        {
            try
            {
                if (!state.cancel.load(std::memory_order_relaxed))
                {
                    processKey(std::move(key));
                }
            }
            catch (const ConvertAbort&)
            {
                if (abortState)
                {
                    abortState->request();
                }
            }
            catch (const std::exception& ex)
            {
                if (diagnostics)
                {
                    diagnostics->error(
                        slang::SourceLocation{},
                        std::string("Convert worker failed: ") + ex.what());
                }
                if (abortState)
                {
                    abortState->request();
                }
            }
            if (diagnostics)
            {
                diagnostics->flushThreadLocal();
            }
            finishTask();
        }
    };

    if (state.pending.load(std::memory_order_relaxed) == 0)
    {
        queue.close();
    }

    for (std::size_t i = 0; i < threadCount; ++i)
    {
        threads.emplace_back(worker);
    }

    {
        std::unique_lock<std::mutex> lock(state.doneMutex);
        state.doneCv.wait(lock, [&]() {
            return state.pending.load(std::memory_order_relaxed) == 0;
        });
    }

    queue.close();
    for (auto& thread : threads)
    {
        thread.join();
    }
    if (diagnostics)
    {
        diagnostics->flushThreadLocal();
    }
    if (state.cancel.load(std::memory_order_relaxed) && abortOnError)
    {
        throw ConvertAbort();
    }
}

void finalizeTopGraphs(wolvrix::lib::grh::Design& design, GraphAssembler& graphAssembler,
                       const TopGraphInfo& info, ConvertContext& context,
                       std::mutex& designMutex)
{
    for (const PlanKey& topKey : info.order)
    {
        std::string_view moduleName;
        if (auto nameIt = info.moduleNames.find(topKey); nameIt != info.moduleNames.end())
        {
            moduleName = nameIt->second;
        }
        const std::string& graphName = graphAssembler.resolveGraphName(topKey, moduleName);
        wolvrix::lib::grh::Graph* graph = design.findGraph(graphName);
        if (!graph)
        {
            if (context.diagnostics)
            {
                context.diagnostics->warn(
                    slang::SourceLocation{},
                    "Skipping top graph alias for missing graph " + graphName);
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(designMutex);
            design.markAsTop(graph->symbol());
        }
        auto aliasIt = info.aliases.find(topKey);
        if (aliasIt == info.aliases.end())
        {
            continue;
        }
        for (const std::string& alias : aliasIt->second)
        {
            if (alias.empty())
            {
                continue;
            }
            const wolvrix::lib::grh::Graph* existing = design.findGraph(alias);
            if (existing && existing->symbol() != graph->symbol())
            {
                if (context.diagnostics)
                {
                    context.diagnostics->warn(
                        slang::SourceLocation{},
                        "Skipping top alias conflict for " + alias);
                }
                continue;
            }
            std::lock_guard<std::mutex> lock(designMutex);
            design.registerGraphAlias(alias, *graph);
        }
    }
}

} // namespace

ConvertDriver::ConvertDriver(ConvertOptions options)
    : options_(options)
{
    logger_.setLevel(options_.logLevel);
    if (options_.enableLogging)
    {
        logger_.enable();
    }
}

wolvrix::lib::grh::Design ConvertDriver::convert(const slang::ast::RootSymbol& root)
{
    wolvrix::lib::grh::Design design;

    planCache_.clear();
    planQueue_.reset();

    ConvertContext context{};
    context.compilation = &root.getCompilation();
    context.root = &root;
    context.options = options_;
    context.diagnostics = &diagnostics_;
    context.logger = &logger_;
    context.planCache = &planCache_;
    context.planQueue = &planQueue_;
    InstanceRegistry instanceRegistry;
    context.instanceRegistry = &instanceRegistry;

    const bool useParallel = !options_.singleThread && options_.threadCount > 1;
    if (useParallel)
    {
        runSlangPrebind(root, context.logger, context.options.enableTiming);
    }

    ConvertParallelState parallel;
    configureParallelContext(context, diagnostics_, useParallel, parallel);
    std::unique_ptr<AbortState> abortState =
        configureAbortHandler(diagnostics_, useParallel, options_.abortOnError,
                              planQueue_, parallel);

    std::mutex designMutex;
    GraphAssembler graphAssembler(context, design, &designMutex);
    TopGraphInfo topInfo = collectTopInstances(root, context);

    auto processKey = [&](PlanKey key) {
        processPlanKey(std::move(key), context, planCache_, graphAssembler);
    };

    if (useParallel)
    {
        runPlanQueueParallel(planQueue_, static_cast<std::size_t>(options_.threadCount),
                             parallel, &diagnostics_, abortState.get(), processKey,
                             options_.abortOnError);
    }
    else
    {
        runPlanQueueSerial(planQueue_, &diagnostics_, processKey);
    }

    finalizeTopGraphs(design, graphAssembler, topInfo, context, designMutex);
    return design;
}

} // namespace wolvrix::lib::ingest
