#include "core/state.hpp"

#include <limits>
#include <stdexcept>

namespace wolvrix::lib::grh {
namespace {
    // State metadata reads do not mutate the graph. Borrow the attributes
    // instead of repeatedly copying an entire declaration and its InitSpec.
    struct StateView {
        const Graph& graph;
        OperationId id;
        OperationKind kind() const { return graph.opKind(id); }
    };

    template <typename T>
    const T* attrRef(const StateView& op, std::string_view name)
    {
        for (const auto& entry : op.graph.opAttrs(op.id)) {
            if (entry.key != name) continue;
            const auto* typed = std::get_if<T>(&entry.value);
            if (!typed) throw std::runtime_error("Wrong state attribute type: " + std::string(name));
            return typed;
        }
        return nullptr;
    }

    template <typename T>
    std::optional<T> attr(const StateView& op, std::string_view name)
    {
        if (const auto* value = attrRef<T>(op, name)) return *value;
        return std::nullopt;
    }

    void requireDeclaration(const StateView& operation)
    {
        if (operation.kind() != OperationKind::kRegister && operation.kind() != OperationKind::kLatch)
            throw std::invalid_argument("State reference does not name a register or latch");
    }

    std::string kindName(InitKind kind)
    {
        switch (kind)
        {
        case InitKind::Constant: return "const";
        case InitKind::Fill: return "fill";
        case InitKind::Readmem: return "readmem";
        case InitKind::Random: return "random";
        }
        throw std::invalid_argument("Unknown InitKind");
    }

    InitKind parseKind(std::string_view text)
    {
        if (text == "const") return InitKind::Constant;
        if (text == "fill") return InitKind::Fill;
        if (text == "readmem") return InitKind::Readmem;
        if (text == "random") return InitKind::Random;
        throw std::invalid_argument("Unknown initializer kind: " + std::string(text));
    }
}

LogicalType stateLogicalType(const Graph& graph, OperationId declaration)
{
    const StateView op{graph, declaration};
    requireDeclaration(op);
    const auto width = attr<int64_t>(op, "width");
    const auto signedness = attr<bool>(op, "isSigned");
    if (!width || *width <= 0 || *width > std::numeric_limits<int32_t>::max() || !signedness)
        throw std::invalid_argument("State requires valid width and isSigned attributes");
    const auto kind = parseValueType(attr<std::string>(op, "valueType").value_or("logic"));
    const auto domain = parseLogicDomain(attr<std::string>(op, "domain").value_or("4state"));
    if (!kind || !domain) throw std::invalid_argument("Unknown state kind/domain");
    LogicalType result{*kind, static_cast<int32_t>(*width), *signedness, *domain,
                       attr<std::vector<int64_t>>(op, "shape").value_or(std::vector<int64_t>{})};
    if (op.kind() == OperationKind::kLatch && !result.shape.empty())
        throw std::invalid_argument("Array latches are not supported");
    return result.normalized();
}

void setStateLogicalType(Graph& graph, OperationId declaration, const LogicalType& type)
{
    requireDeclaration({graph, declaration});
    const LogicalType normalized = type.normalized();
    if (graph.opKind(declaration) == OperationKind::kLatch && !normalized.shape.empty())
        throw std::invalid_argument("Array latches are not supported");
    graph.setAttr(declaration, "width", static_cast<int64_t>(normalized.width));
    graph.setAttr(declaration, "isSigned", normalized.isSigned);
    graph.setAttr(declaration, "valueType", std::string(toString(normalized.kind)));
    graph.setAttr(declaration, "domain", std::string(toString(normalized.domain)));
    if (normalized.shape.empty()) graph.eraseAttr(declaration, "shape");
    else graph.setAttr(declaration, "shape", normalized.shape);
}

void validateInitSpec(const LogicalType& type, const InitSpec& steps)
{
    type.validate();
    if (steps.empty()) throw std::invalid_argument("State initialization must not be empty");
    const int64_t rows = type.kind == ValueType::Logic ? type.width / type.leafWidth() : 1;
    for (const InitStep& step : steps)
    {
        kindName(step.kind);
        if (step.start && (*step.start < 0 || *step.start > rows))
            throw std::invalid_argument("Initializer start is outside the state");
        if (step.count && (*step.count < 0 || *step.count > rows - step.start.value_or(0)))
            throw std::invalid_argument("Initializer count is outside the state");
        if (step.kind == InitKind::Readmem && (type.shape.empty() || step.file.empty() ||
            (step.format != "hex" && step.format != "bin")))
            throw std::invalid_argument("readmem requires an array, a file and hex/bin format");
        if (step.kind == InitKind::Fill && type.shape.empty())
            throw std::invalid_argument("fill initializer requires an array");
        if ((step.kind == InitKind::Constant || (step.kind == InitKind::Fill && !step.random)) &&
            step.value.empty() && type.kind != ValueType::String)
            throw std::invalid_argument("Initializer is missing its value");
        if (step.random && step.kind != InitKind::Fill)
            throw std::invalid_argument("random flag belongs to fill initializers");
        if (step.seed && step.kind != InitKind::Random && !step.random)
            throw std::invalid_argument("Seed belongs to random initializers");
    }
}

InitSpec stateInitSpec(const Graph& graph, OperationId declaration)
{
    const StateView op{graph, declaration};
    const LogicalType type = stateLogicalType(graph, declaration);
    if (const auto* kinds = attrRef<std::vector<std::string>>(op, "initSpecKinds"))
    {
        const auto* values = attrRef<std::vector<std::string>>(op, "initSpecValues");
        const auto* files = attrRef<std::vector<std::string>>(op, "initSpecFiles");
        const auto* formats = attrRef<std::vector<std::string>>(op, "initSpecFormats");
        const auto* starts = attrRef<std::vector<int64_t>>(op, "initSpecStarts");
        const auto* counts = attrRef<std::vector<int64_t>>(op, "initSpecCounts");
        const auto* seeds = attrRef<std::vector<int64_t>>(op, "initSpecSeeds");
        const auto* hasSeeds = attrRef<std::vector<bool>>(op, "initSpecHasSeeds");
        const auto* random = attrRef<std::vector<bool>>(op, "initSpecRandom");
        const auto n = kinds->size();
        if (!values || !files || !formats || !starts || !counts || !seeds || !hasSeeds || !random ||
            values->size() != n || files->size() != n || formats->size() != n || starts->size() != n ||
            counts->size() != n || seeds->size() != n || hasSeeds->size() != n || random->size() != n)
            throw std::invalid_argument("Incomplete typed InitSpec attributes");
        InitSpec result;
        result.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            if ((*starts)[i] < -1 || (*counts)[i] < -1)
                throw std::invalid_argument("Invalid InitSpec range encoding");
            result.push_back({parseKind((*kinds)[i]), (*values)[i], (*files)[i], (*formats)[i],
                              (*starts)[i] < 0 ? std::nullopt : std::optional((*starts)[i]),
                              (*counts)[i] < 0 ? std::nullopt : std::optional((*counts)[i]),
                              (*hasSeeds)[i] ? std::optional((*seeds)[i]) : std::nullopt, (*random)[i]});
        }
        validateInitSpec(type, result);
        return result;
    }
    const std::string defaultValue = type.kind == ValueType::String ? "" :
        type.kind == ValueType::Real || type.domain == LogicDomain::TwoState ? "0" : "x";
    InitSpec result;
    if (type.shape.empty())
    {
        const auto value = attr<std::string>(op, "initValue").value_or(defaultValue);
        result.push_back({value == "$random" ? InitKind::Random : InitKind::Constant,
                          value == "$random" ? std::string() : value});
    }
    else
    {
        result.push_back({InitKind::Fill, defaultValue});
        const auto kinds = attr<std::vector<std::string>>(op, "initKind").value_or(std::vector<std::string>{});
        const auto values = attr<std::vector<std::string>>(op, "initValue").value_or(std::vector<std::string>{});
        const auto files = attr<std::vector<std::string>>(op, "initFile").value_or(std::vector<std::string>{});
        const auto starts = attr<std::vector<int64_t>>(op, "initStart").value_or(std::vector<int64_t>{});
        const auto counts = attr<std::vector<int64_t>>(op, "initLen").value_or(std::vector<int64_t>{});
        for (std::size_t i = 0; i < kinds.size(); ++i)
        {
            InitStep step;
            if (kinds[i] == "literal")
            {
                step.kind = InitKind::Fill;
                step.value = i < values.size() && !values[i].empty() ? values[i] : defaultValue;
                step.random = step.value == "$random";
                if (step.random) step.value.clear();
            }
            else if (kinds[i] == "readmemh" || kinds[i] == "readmemb")
            {
                step.kind = InitKind::Readmem;
                step.format = kinds[i] == "readmemh" ? "hex" : "bin";
                step.file = i < files.size() ? files[i] : std::string();
            }
            else throw std::invalid_argument("Unknown legacy initializer kind: " + kinds[i]);
            if (i < starts.size() && starts[i] >= 0) step.start = starts[i];
            if (i < counts.size() && counts[i] > 0) step.count = counts[i];
            result.push_back(std::move(step));
        }
    }
    validateInitSpec(type, result);
    return result;
}

void setStateInitSpec(Graph& graph, OperationId declaration, const InitSpec& steps)
{
    validateInitSpec(stateLogicalType(graph, declaration), steps);
    std::vector<std::string> kinds, values, files, formats;
    std::vector<int64_t> starts, counts, seeds;
    std::vector<bool> hasSeeds, random;
    for (const InitStep& step : steps)
    {
        kinds.push_back(kindName(step.kind));
        values.push_back(step.value);
        files.push_back(step.file);
        formats.push_back(step.format);
        starts.push_back(step.start.value_or(-1));
        counts.push_back(step.count.value_or(-1));
        seeds.push_back(step.seed.value_or(0));
        hasSeeds.push_back(step.seed.has_value());
        random.push_back(step.random);
    }
    graph.setAttr(declaration, "initSpecKinds", std::move(kinds));
    graph.setAttr(declaration, "initSpecValues", std::move(values));
    graph.setAttr(declaration, "initSpecFiles", std::move(files));
    graph.setAttr(declaration, "initSpecFormats", std::move(formats));
    graph.setAttr(declaration, "initSpecStarts", std::move(starts));
    graph.setAttr(declaration, "initSpecCounts", std::move(counts));
    graph.setAttr(declaration, "initSpecSeeds", std::move(seeds));
    graph.setAttr(declaration, "initSpecHasSeeds", std::move(hasSeeds));
    graph.setAttr(declaration, "initSpecRandom", std::move(random));
    for (const auto key : {"initKind", "initValue", "initFile", "initStart", "initLen"})
        graph.eraseAttr(declaration, key);
}

} // namespace wolvrix::lib::grh
