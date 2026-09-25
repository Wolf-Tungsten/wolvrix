#include "grhsim/ir/verifier.hpp"
#include "grhsim/backend/cpu.hpp"

#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/model.hpp"

#include <cmath>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        template <typename IdType>
        bool validId(IdType id, std::size_t size) noexcept
        {
            return id.generation == 0 && id.index != 0 && id.index <= size;
        }

        bool validOptionalString(const GrhSimModel &model, StringId id) noexcept
        {
            return !id.valid() || model.strings().valid(id);
        }

        bool parameterValueValid(const ParameterValue &value)
        {
            return std::visit([](const auto &entry) {
                using T = std::decay_t<decltype(entry)>;
                if constexpr (std::is_same_v<T, double>)
                {
                    return std::isfinite(entry);
                }
                else if constexpr (std::is_same_v<T, std::vector<double>>)
                {
                    for (double item : entry)
                    {
                        if (!std::isfinite(item)) return false;
                    }
                    return true;
                }
                else
                {
                    return true;
                }
            }, value);
        }

        template <typename ContextFactory>
        bool validParameters(const GrhSimModel &model,
                             std::span<const Parameter> parameters,
                             diag::Diagnostics &diagnostics,
                             ContextFactory context)
        {
            bool ok = true;
            for (std::size_t i = 0; i < parameters.size(); ++i)
            {
                const Parameter &parameter = parameters[i];
                if (!model.strings().valid(parameter.name))
                {
                    diagnostics.error("parameter has an invalid name StringId", context());
                    ok = false;
                }
                else
                {
                    for (std::size_t j = 0; j < i; ++j)
                    {
                        if (parameters[j].name != parameter.name) continue;
                        diagnostics.error("parameter name is duplicated: " +
                                          std::string(model.text(parameter.name)), context());
                        ok = false;
                        break;
                    }
                }
                if (!parameterValueValid(parameter.value))
                {
                    diagnostics.error("parameter contains a non-finite floating-point value",
                                      context());
                    ok = false;
                }
            }
            return ok;
        }

        const Parameter *findParameter(const GrhSimModel &model,
                                       std::span<const Parameter> parameters,
                                       std::string_view name)
        {
            for (const Parameter &parameter : parameters)
            {
                if (model.text(parameter.name) == name) return &parameter;
            }
            return nullptr;
        }
    } // namespace

    bool verifyGrhSimModel(const GrhSimModel &model,
                           const DialectRegistry &registry,
                           diag::Diagnostics &diagnostics)
    {
        bool ok = true;
        auto error = [&](std::string message, std::string context = {}) {
            diagnostics.error(std::move(message), std::move(context));
            ok = false;
        };

        if (model.poisoned())
        {
            error("model is poisoned by a failed in-place pass", "model");
            return false;
        }
        if (!model.strings().valid(model.name()))
        {
            error("model name must be a valid non-empty StringId", "model");
        }

        std::unordered_set<std::string> manifestNames;
        for (const DialectUse &use : model.dialects())
        {
            if (!model.strings().valid(use.name) || !model.strings().valid(use.version))
            {
                error("dialect manifest contains invalid string references", "dialects");
                continue;
            }
            const std::string name(model.text(use.name));
            if (!manifestNames.insert(name).second)
            {
                error("dialect manifest contains duplicate entry: " + name, "dialects");
                continue;
            }
            const DialectDefinition *definition = registry.findDialect(name);
            if (!definition)
            {
                error("required dialect is not registered: " + name, "dialects");
                continue;
            }
            if (definition->version != model.text(use.version))
            {
                error("dialect version mismatch for " + name + ": model=" +
                      std::string(model.text(use.version)) + " registry=" + definition->version,
                      "dialects");
            }
            if (use.schemaFingerprint.valid() &&
                definition->schemaFingerprint != model.text(use.schemaFingerprint))
            {
                error("dialect schema fingerprint mismatch for " + name, "dialects");
            }
        }
        if (!manifestNames.contains("core"))
        {
            error("core dialect is missing from the manifest", "dialects");
        }

        for (std::size_t i = 0; i < model.types().size(); ++i)
        {
            const Type &type = model.types()[i];
            const std::string context = "types[" + std::to_string(i) + "]";
            if (type.id != TypeId{static_cast<uint32_t>(i + 1), 0})
                error("type ID does not match stable table position", context);
            if (!model.strings().valid(type.typeRef) || !registry.hasType(model.text(type.typeRef)))
                error("type has an unknown dialect reference", context);
            else if (model.text(type.typeRef).starts_with("cpu."))
                error("CPU types may only occur in backend mappings", context);
            switch (type.kind)
            {
            case TypeKind::Logic:
                if (type.width == 0) error("core.logic width must be greater than zero", context);
                if (type.elementType.valid() || type.count != 0)
                    error("core.logic has array-only fields", context);
                break;
            case TypeKind::Array:
                if (!validId(type.elementType, model.types().size()))
                    error("core.array element type is invalid", context);
                if (type.width != 0) error("core.array has logic-only width", context);
                break;
            case TypeKind::Real:
            case TypeKind::String:
                if (type.width != 0 || type.elementType.valid() || type.count != 0)
                    error("scalar non-logic type has incompatible fields", context);
                break;
            }
        }

        auto verifyOriginRef = [&](OriginId id, std::string_view context) {
            if (id.valid() && !validId(id, model.origins().size()))
                error("origin ID is out of range", std::string(context));
        };
        for (std::size_t i = 0; i < model.origins().size(); ++i)
        {
            const Origin &origin = model.origins()[i];
            const std::string context = "origins[" + std::to_string(i) + "]";
            if (origin.id != OriginId{static_cast<uint32_t>(i + 1), 0})
                error("origin ID does not match stable table position", context);
            for (StringId id : {origin.sourceKind, origin.symbol, origin.file, origin.pass, origin.note})
            {
                if (!validOptionalString(model, id)) error("origin has an invalid StringId", context);
            }
        }

        for (std::size_t i = 0; i < model.inputs().size(); ++i)
        {
            const InputObject &object = model.inputs()[i];
            const std::string context = "inputs[" + std::to_string(i) + "]";
            if (object.id != InputId{static_cast<uint32_t>(i + 1), 0})
                error("input ID does not match stable table position", context);
            if (!model.strings().valid(object.name)) error("input name is invalid", context);
            if (!validId(object.type, model.types().size())) error("input type is invalid", context);
            verifyOriginRef(object.origin, context);
        }
        for (std::size_t i = 0; i < model.outputs().size(); ++i)
        {
            const OutputObject &object = model.outputs()[i];
            const std::string context = "outputs[" + std::to_string(i) + "]";
            if (object.id != OutputId{static_cast<uint32_t>(i + 1), 0})
                error("output ID does not match stable table position", context);
            if (!model.strings().valid(object.name)) error("output name is invalid", context);
            if (!validId(object.type, model.types().size())) error("output type is invalid", context);
            verifyOriginRef(object.origin, context);
        }
        for (std::size_t i = 0; i < model.states().size(); ++i)
        {
            const StateObject &object = model.states()[i];
            const auto context = [i] { return "states[" + std::to_string(i) + "]"; };
            if (object.id != StateId{static_cast<uint32_t>(i + 1), 0})
                error("state ID does not match stable table position", context());
            if (!model.strings().valid(object.name)) error("state name is invalid", context());
            if (!validId(object.type, model.types().size())) error("state type is invalid", context());
            if (object.origin.valid()) verifyOriginRef(object.origin, context());
        }

        for (std::size_t i = 0; i < model.functions().size(); ++i)
        {
            const ExternFunction &function = model.functions()[i];
            const std::string context = "functions[" + std::to_string(i) + "]";
            if (function.id != FuncId{static_cast<uint32_t>(i + 1), 0})
                error("function ID does not match stable table position", context);
            if (!model.strings().valid(function.name) || !model.strings().valid(function.symbol))
                error("function name or symbol is invalid", context);
            if (!model.strings().valid(function.declRef) ||
                !registry.hasFunctionDecl(model.text(function.declRef)))
                error("function declaration reference is unknown", context);
            if (function.returnType.valid() && !validId(function.returnType, model.types().size()))
                error("function return type is invalid", context);
            verifyOriginRef(function.origin, context);
            try
            {
                for (const DpiArgument &argument : model.arguments(function))
                {
                    if (!model.strings().valid(argument.name) ||
                        !validId(argument.type, model.types().size()))
                        error("function argument name or type is invalid", context);
                }
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context);
            }
        }

        for (std::size_t i = 0; i < model.interfacePorts().size(); ++i)
        {
            const InterfacePort &port = model.interfacePorts()[i];
            const std::string context = "interface[" + std::to_string(i) + "]";
            if (!model.strings().valid(port.name)) error("interface port name is invalid", context);
            if (port.direction == InterfaceDirection::Input)
            {
                if (!validId(port.input, model.inputs().size()) || port.output.valid() ||
                    port.outputEnable.valid())
                    error("input interface port has invalid object bindings", context);
            }
            else if (port.direction == InterfaceDirection::Output)
            {
                if (!validId(port.output, model.outputs().size()) || port.input.valid() ||
                    port.outputEnable.valid())
                    error("output interface port has invalid object bindings", context);
            }
            else if (!validId(port.input, model.inputs().size()) ||
                     !validId(port.output, model.outputs().size()) ||
                     !validId(port.outputEnable, model.outputs().size()))
            {
                error("inout interface port must bind input, output, and output-enable objects", context);
            }
        }

        std::vector<uint32_t> producers(model.values().size(), 0);
        for (std::size_t i = 0; i < model.values().size(); ++i)
        {
            const SimValue &value = model.values()[i];
            const auto context = [i] { return "values[" + std::to_string(i) + "]"; };
            if (value.id != ValueId{static_cast<uint32_t>(i + 1), 0})
                error("value ID does not match stable table position", context());
            if (!validId(value.type, model.types().size())) error("value type is invalid", context());
            if (!validOptionalString(model, value.name)) error("value name is invalid", context());
            if (value.origin.valid()) verifyOriginRef(value.origin, context());
        }

        auto objectRefValid = [&](ObjectRef ref) {
            if (ref.generation != 0 || ref.index == 0) return false;
            switch (ref.kind)
            {
            case ObjectKind::Input: return ref.index <= model.inputs().size();
            case ObjectKind::Output: return ref.index <= model.outputs().size();
            case ObjectKind::State: return ref.index <= model.states().size();
            case ObjectKind::Function: return ref.index <= model.functions().size();
            }
            return false;
        };

        std::vector<int8_t> opReferenceValidity(model.strings().size() + 1, 0);
        for (std::size_t i = 0; i < model.operations().size(); ++i)
        {
            const SimOp &op = model.operations()[i];
            const auto context = [i] { return "operations[" + std::to_string(i) + "]"; };
            if (op.id != OpId{static_cast<uint32_t>(i + 1), 0})
                error("operation ID does not match stable table position", context());
            if (!model.strings().valid(op.opType))
                error("operation has an invalid dialect reference", context());
            else
            {
                int8_t &validity = opReferenceValidity[op.opType.index];
                if (validity == 0) validity = registry.hasOp(model.text(op.opType)) ? 1 : -1;
                if (validity < 0) error("operation has an unknown dialect reference", context());
            }
            if (!validOptionalString(model, op.name)) error("operation name is invalid", context());
            if (op.origin.valid()) verifyOriginRef(op.origin, context());
            try
            {
                const auto operands = model.operands(op);
                const auto results = model.results(op);
                const auto refs = model.objectRefs(op);
                const auto parameters = model.parameters(op);
                for (ValueId value : operands)
                    if (!validId(value, model.values().size())) error("operand value ID is invalid", context());
                for (ValueId value : results)
                {
                    if (!validId(value, model.values().size()))
                        error("result value ID is invalid", context());
                    else
                        ++producers[value.index - 1];
                }
                for (ObjectRef ref : refs)
                    if (!objectRefValid(ref)) error("object reference is invalid", context());
                validParameters(model, parameters, diagnostics, context);

                const std::string_view opName = model.text(op.opType);
                if (opName == "core.input.read")
                {
                    if (!operands.empty() || results.size() != 1 || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::Input)
                        error("core.input.read must have 0 operands, 1 result and 1 input ref", context());
                }
                else if (opName == "core.output.write")
                {
                    if (operands.size() != 1 || !results.empty() || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::Output)
                        error("core.output.write must have 1 operand, 0 results and 1 output ref", context());
                }
                else if (opName == "core.state.read" || opName == "core.state.memRead")
                {
                    const std::size_t expectedOperands = opName == "core.state.read" ? 0 : 1;
                    if (operands.size() != expectedOperands || results.size() != 1 || refs.size() != 1 ||
                        refs[0].kind != ObjectKind::State)
                        error("core state read operation has an invalid shape", context());
                }
                else if (opName == "core.system.function" && results.size() != 1)
                {
                    error("core.system.function must have exactly one result", context());
                }
                else if (opName == "core.dpi.call" &&
                         (refs.empty() || refs.front().kind != ObjectKind::Function))
                {
                    error("core.dpi.call must reference its function first", context());
                }
                else if (opName == "core.compute.bitSelect") {
                    bool valid = operands.size() == 3 && results.size() == 1 &&
                                 refs.empty() && parameters.empty();
                    TypeId typeId;
                    const auto scalar = [&](ValueId value) {
                        if (!validId(value, model.values().size())) return false;
                        const auto id = model.values()[value.index - 1].type;
                        if (!validId(id, model.types().size())) return false;
                        const auto &type = model.types()[id.index - 1];
                        if (!typeId) typeId = id;
                        return id == typeId && type.kind == TypeKind::Logic &&
                               type.domain == LogicDomain::TwoState && type.width > 0 && type.width <= 64;
                    };
                    for (const auto value : operands) valid &= scalar(value);
                    for (const auto value : results) valid &= scalar(value);
                    if (!valid) error("bitSelect requires three operands and one result of the same scalar two-state type", context());
                }
                else if (opName == "core.compute.prioritySelect") {
                    // [c0..cN-1, a0..aN-1, default]: the first true condition wins.
                    bool valid = results.size() == 1 && refs.empty() && parameters.empty() &&
                                 operands.size() >= 7 && operands.size() % 2 == 1;
                    if (valid) {
                        const std::size_t count = (operands.size() - 1) / 2;
                        valid = count <= 64;
                        const auto valueType = [&](ValueId value) -> const Type * {
                            if (!validId(value, model.values().size())) return nullptr;
                            const auto id = model.values()[value.index - 1].type;
                            if (!validId(id, model.types().size())) return nullptr;
                            return &model.types()[id.index - 1];
                        };
                        const auto *resultType = valueType(results[0]);
                        valid &= resultType && resultType->kind == TypeKind::Logic &&
                                 resultType->domain == LogicDomain::TwoState &&
                                 resultType->width > 0 && resultType->width <= 64;
                        for (std::size_t i = 0; i < count && valid; ++i) {
                            const auto *condType = valueType(operands[i]);
                            valid &= condType && condType->kind == TypeKind::Logic &&
                                     condType->domain == LogicDomain::TwoState &&
                                     condType->width == 1 && !condType->isSigned;
                        }
                        for (std::size_t i = count; i < operands.size() && valid; ++i) {
                            const auto *armType = valueType(operands[i]);
                            valid &= armType && armType->kind == TypeKind::Logic &&
                                     armType->domain == LogicDomain::TwoState &&
                                     armType->width > 0 && armType->width <= 64;
                        }
                    }
                    if (!valid) error("prioritySelect requires 2N+1 operands (3<=N<=64): N one-bit unsigned two-state conditions, N arms and one default of scalar two-state types, and one result", context());
                }
                else if (opName == "core.compute.expr") {
                    // Fused expression tree (grhsim.fuse-expr-chains): operands are the
                    // tree's leaf values; the "tree" parameter holds postfix tokens —
                    // leaf "l<k>" (operand k) or node
                    // "n;<kind>;<width>;<signed01>;<arity>;<opId>[;key=int64...]" — and
                    // "rk" names the root's original op kind for dynamic accounting.
                    // The root token is last and the stack must reduce to one value.
                    bool valid = results.size() == 1 && refs.empty();
                    const Parameter *tree = findParameter(model, parameters, "tree");
                    const Parameter *rk = findParameter(model, parameters, "rk");
                    const std::vector<std::string> *tokens = nullptr;
                    if (!tree || !std::holds_alternative<std::vector<std::string>>(tree->value) ||
                        (tokens = &std::get<std::vector<std::string>>(tree->value))->empty())
                        valid = false;
                    if (!rk || !std::holds_alternative<std::string>(rk->value) ||
                        !std::get<std::string>(rk->value).starts_with("core.compute.") ||
                        std::get<std::string>(rk->value) == "core.compute.expr")
                        valid = false;
                    if (valid) {
                        const auto parseUnsigned = [](std::string_view text, std::uint64_t &out) {
                            if (text.empty()) return false;
                            for (const char ch : text) if (ch < '0' || ch > '9') return false;
                            out = std::stoull(std::string(text));
                            return true;
                        };
                        std::size_t depth = 0;
                        for (const auto &token : *tokens) {
                            if (!valid) break;
                            if (token.size() > 1 && token[0] == 'l') {
                                std::uint64_t leaf = 0;
                                if (!parseUnsigned(std::string_view(token).substr(1), leaf) ||
                                    leaf >= operands.size()) { valid = false; break; }
                                ++depth;
                                continue;
                            }
                            std::vector<std::string_view> fields;
                            std::size_t begin = 0;
                            for (std::size_t i = 0; i <= token.size(); ++i)
                                if (i == token.size() || token[i] == ';') {
                                    fields.push_back(std::string_view(token).substr(begin, i - begin));
                                    begin = i + 1;
                                }
                            std::uint64_t width = 0, arity = 0, origin = 0;
                            if (fields.size() < 6 || fields[0] != "n" || fields[1].empty() ||
                                fields[1] == "expr" || fields[1].find_first_of(";= ") != std::string_view::npos ||
                                !parseUnsigned(fields[2], width) || width == 0 || width > 64 ||
                                (fields[3] != "0" && fields[3] != "1") ||
                                !parseUnsigned(fields[4], arity) || arity == 0 || arity > depth ||
                                !parseUnsigned(fields[5], origin) || origin == 0 ||
                                origin > model.operations().size()) { valid = false; break; }
                            for (std::size_t i = 6; i < fields.size() && valid; ++i) {
                                const auto eq = fields[i].find('=');
                                std::string_view number = eq == std::string_view::npos ? std::string_view{} :
                                    fields[i].substr(eq + 1);
                                if (!number.empty() && number.front() == '-') number.remove_prefix(1);
                                std::uint64_t ignored = 0;
                                if (eq == std::string_view::npos || eq == 0 || !parseUnsigned(number, ignored))
                                    valid = false;
                            }
                            depth = depth - arity + 1;
                        }
                        if (depth != 1 || (*tokens).back()[0] != 'n') valid = false;
                        const auto scalar = [&](ValueId value) {
                            if (!validId(value, model.values().size())) return false;
                            const auto id = model.values()[value.index - 1].type;
                            if (!validId(id, model.types().size())) return false;
                            const auto &type = model.types()[id.index - 1];
                            return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                                   type.width > 0 && type.width <= 64;
                        };
                        for (const auto value : operands) valid &= scalar(value);
                        for (const auto value : results) valid &= scalar(value);
                    }
                    if (!valid) error("core.compute.expr requires one scalar two-state result, scalar two-state leaf operands, a postfix \"tree\" string-array parameter reducing to one value and a \"rk\" string parameter naming the root's original op kind", context());
                }

                const Parameter *edges = findParameter(model, parameters, "event_edges");
                if (edges && !std::holds_alternative<std::vector<std::string>>(edges->value))
                    error("event_edges must be a string array", context());
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context());
            }
        }
        for (std::size_t i = 0; i < producers.size(); ++i)
        {
            if (producers[i] != 1)
                error("value must have exactly one producer; observed " +
                      std::to_string(producers[i]), "values[" + std::to_string(i) + "]");
        }

        std::vector<uint32_t> initCount(model.states().size(), 0);
        for (std::size_t i = 0; i < model.initRecords().size(); ++i)
        {
            const InitRecord &record = model.initRecords()[i];
            const auto context = [i] { return "init[" + std::to_string(i) + "]"; };
            if (!validId(record.state, model.states().size()))
            {
                error("init record state ID is invalid", context());
                continue;
            }
            ++initCount[record.state.index - 1];
            try
            {
                const auto steps = model.steps(record);
                if (steps.empty()) error("InitSpec must contain at least one step", context());
                for (const InitStep &step : steps)
                {
                    if (!model.strings().valid(step.kind) ||
                        !registry.hasInitStep(model.text(step.kind)))
                        error("init step has an unknown dialect reference", context());
                    validParameters(model, model.parameters(step), diagnostics, context);
                }
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context());
            }
        }
        for (std::size_t i = 0; i < initCount.size(); ++i)
        {
            if (initCount[i] != 1)
                error("state must have exactly one InitSpec; observed " +
                      std::to_string(initCount[i]), "states[" + std::to_string(i) + "]");
        }

        std::unordered_set<uint32_t> mappingBackends;
        const bool validModel = ok && !diagnostics.hasError();
        for (std::size_t i = 0; i < model.mappings().size(); ++i)
        {
            const BackendMapping &mapping = model.mappings()[i];
            const std::string context = "mappings[" + std::to_string(i) + "]";
            if (!model.strings().valid(mapping.backend) || !model.strings().valid(mapping.schema))
                error("mapping backend or schema is invalid", context);
            if (!mappingBackends.insert(mapping.backend.index).second)
                error("mapping backend is duplicated", context);
            if (mapping.sourceIdentity != model.identity() ||
                mapping.sourceSemanticRevision != model.semanticRevision())
                error("mapping is stale for the current model revision", context);
            try
            {
                validParameters(model, model.parameters(mapping), diagnostics,
                                [&context] { return context; });
                if (validModel && (mapping.cpu || model.text(mapping.backend) == "cpu"))
                    if (!verifyCpuMapping(model, mapping, diagnostics)) ok = false;
            }
            catch (const std::exception &ex)
            {
                error(ex.what(), context);
            }
        }

        return ok && !diagnostics.hasError();
    }

} // namespace wolvrix::lib::grhsim
