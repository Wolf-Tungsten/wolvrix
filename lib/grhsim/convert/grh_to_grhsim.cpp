#include "grhsim/convert/grh_to_grhsim.hpp"

#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/verifier.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        using namespace wolvrix::lib::grh;

        template <typename T>
        std::optional<T> attr(const Operation &op, std::string_view name)
        {
            const auto value = op.attr(name);
            if (!value) return std::nullopt;
            if (const auto *typed = std::get_if<T>(&*value)) return *typed;
            return std::nullopt;
        }

        ParameterValue copyAttribute(const AttributeValue &value)
        {
            return std::visit([](const auto &entry) -> ParameterValue { return entry; }, value);
        }

        bool isDeclaration(OperationKind kind)
        {
            return kind == OperationKind::kRegister || kind == OperationKind::kLatch ||
                   kind == OperationKind::kDpicImport;
        }

        bool isForbiddenHierarchy(OperationKind kind)
        {
            return kind == OperationKind::kInstance || kind == OperationKind::kBlackbox ||
                   kind == OperationKind::kXMRRead || kind == OperationKind::kXMRWrite;
        }

        std::optional<std::string_view> computeOpName(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kConstant: return "core.compute.constant";
            case OperationKind::kAdd: return "core.compute.add";
            case OperationKind::kSub: return "core.compute.sub";
            case OperationKind::kMul: return "core.compute.mul";
            case OperationKind::kDiv: return "core.compute.div";
            case OperationKind::kMod: return "core.compute.mod";
            case OperationKind::kEq: return "core.compute.eq";
            case OperationKind::kNe: return "core.compute.ne";
            case OperationKind::kCaseEq: return "core.compute.caseEq";
            case OperationKind::kCaseNe: return "core.compute.caseNe";
            case OperationKind::kWildcardEq: return "core.compute.wildcardEq";
            case OperationKind::kWildcardNe: return "core.compute.wildcardNe";
            case OperationKind::kLt: return "core.compute.lt";
            case OperationKind::kLe: return "core.compute.le";
            case OperationKind::kGt: return "core.compute.gt";
            case OperationKind::kGe: return "core.compute.ge";
            case OperationKind::kAnd: return "core.compute.and";
            case OperationKind::kOr: return "core.compute.or";
            case OperationKind::kXor: return "core.compute.xor";
            case OperationKind::kXnor: return "core.compute.xnor";
            case OperationKind::kNot: return "core.compute.not";
            case OperationKind::kLogicAnd: return "core.compute.logicAnd";
            case OperationKind::kLogicOr: return "core.compute.logicOr";
            case OperationKind::kLogicNot: return "core.compute.logicNot";
            case OperationKind::kReduceAnd: return "core.compute.reduceAnd";
            case OperationKind::kReduceOr: return "core.compute.reduceOr";
            case OperationKind::kReduceXor: return "core.compute.reduceXor";
            case OperationKind::kReduceNor: return "core.compute.reduceNor";
            case OperationKind::kReduceNand: return "core.compute.reduceNand";
            case OperationKind::kReduceXnor: return "core.compute.reduceXnor";
            case OperationKind::kShl: return "core.compute.shl";
            case OperationKind::kLShr: return "core.compute.lshr";
            case OperationKind::kAShr: return "core.compute.ashr";
            case OperationKind::kMux: return "core.compute.mux";
            case OperationKind::kAssign: return "core.compute.assign";
            case OperationKind::kConcat: return "core.compute.concat";
            case OperationKind::kReplicate: return "core.compute.replicate";
            case OperationKind::kSliceStatic: return "core.compute.sliceStatic";
            case OperationKind::kSliceDynamic: return "core.compute.sliceDynamic";
            case OperationKind::kSliceArray: return "core.compute.sliceArray";
            default: return std::nullopt;
            }
        }

        std::optional<std::string_view> loweredOpName(OperationKind kind)
        {
            if (auto compute = computeOpName(kind)) return compute;
            switch (kind)
            {
            case OperationKind::kRegisterReadPort:
            case OperationKind::kLatchReadPort: return "core.state.read";
            // stage-03: the GRH kMemory family is unified into kRegister; the
            // grhsim mem* op names stay until the stage-06 backend rework.
            case OperationKind::kRegisterReadArrayPort: return "core.state.memRead";
            case OperationKind::kRegisterWritePort:
            case OperationKind::kRegisterWriteMaskPort: return "core.state.regWrite";
            case OperationKind::kRegisterWriteArrayPort:
            case OperationKind::kRegisterWriteArrayMaskPort: return "core.state.memWrite";
            case OperationKind::kRegisterFillPort: return "core.state.memFill";
            case OperationKind::kLatchWritePort: return "core.state.latchWrite";
            case OperationKind::kSystemFunction: return "core.system.function";
            case OperationKind::kSystemTask: return "core.system.task";
            case OperationKind::kDpicCall: return "core.dpi.call";
            default: return std::nullopt;
            }
        }

        std::string_view targetAttribute(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterReadArrayPort:
            case OperationKind::kRegisterWritePort:
            case OperationKind::kRegisterWriteMaskPort:
            case OperationKind::kRegisterWriteArrayPort:
            case OperationKind::kRegisterWriteArrayMaskPort:
            case OperationKind::kRegisterFillPort: return "regSymbol";
            case OperationKind::kLatchReadPort:
            case OperationKind::kLatchWritePort: return "latchSymbol";
            case OperationKind::kDpicCall: return "targetImportSymbol";
            default: return {};
            }
        }

        bool isEventSensitive(OperationKind kind)
        {
            return kind == OperationKind::kRegisterWritePort ||
                   kind == OperationKind::kRegisterWriteMaskPort ||
                   kind == OperationKind::kRegisterWriteArrayPort ||
                   kind == OperationKind::kRegisterWriteArrayMaskPort ||
                   kind == OperationKind::kRegisterFillPort ||
                   kind == OperationKind::kSystemTask || kind == OperationKind::kDpicCall;
        }

        std::string_view normalizedParameterName(std::string_view name)
        {
            if (name == "eventEdge") return "event_edges";
            if (name == "hasSideEffects") return "has_side_effects";
            if (name == "procKind") return "proc_kind";
            if (name == "hasTiming") return "has_timing";
            return name;
        }

        const Graph *selectTop(const Design &design, const GrhToGrhSimOptions &options,
                               diag::Diagnostics &diagnostics)
        {
            if (!options.top.empty())
            {
                const Graph *graph = design.findGraph(options.top);
                if (!graph) diagnostics.error("GRH top graph not found: " + options.top, "lower_grhsim");
                return graph;
            }
            if (design.topGraphs().size() == 1)
            {
                return design.findGraph(design.topGraphs().front());
            }
            if (design.topGraphs().empty() && design.graphs().size() == 1)
            {
                return design.graphs().begin()->second.get();
            }
            diagnostics.error("GRH to GrhSIM lowering requires exactly one explicit top",
                              "lower_grhsim");
            return nullptr;
        }

        TypeId valueType(GrhSimModel &model, const Graph &graph, grh::ValueId value,
                         LogicDomain domain)
        {
            switch (graph.valueType(value))
            {
            case ValueType::Real: return model.realType();
            case ValueType::String: return model.stringType();
            case ValueType::Logic:
            default:
                return model.logicType(static_cast<uint32_t>(std::max<int32_t>(1, graph.valueWidth(value))),
                                       graph.valueSigned(value), domain);
            }
        }

        ValueType parseDpiValueType(std::string_view name)
        {
            if (name == "real" || name == "shortreal") return ValueType::Real;
            if (name == "string") return ValueType::String;
            return ValueType::Logic;
        }

        TypeId scalarType(GrhSimModel &model, ValueType valueKind, int64_t width,
                          bool isSigned, LogicDomain domain)
        {
            if (valueKind == ValueType::Real) return model.realType();
            if (valueKind == ValueType::String) return model.stringType();
            return model.logicType(static_cast<uint32_t>(std::max<int64_t>(1, width)),
                                   isSigned, domain);
        }

        OriginId makeOrigin(GrhSimModel &model, bool enabled, std::string_view sourceKind,
                            std::string_view symbol, uint32_t index,
                            const std::optional<SrcLoc> &location)
        {
            if (!enabled) return {};
            Origin origin;
            origin.sourceKind = model.intern(sourceKind);
            origin.symbol = model.intern(symbol);
            origin.sourceIndex = index;
            if (location)
            {
                origin.file = model.intern(location->file);
                origin.line = location->line;
                origin.column = location->column;
                origin.endLine = location->endLine;
                origin.endColumn = location->endColumn;
                origin.pass = model.intern(location->pass);
                origin.note = model.intern(location->note);
            }
            return model.addOrigin(origin);
        }

        void appendInitStep(GrhSimModel &model, std::vector<InitStep> &steps,
                            std::vector<Parameter> &parameters, std::string_view kind,
                            std::vector<std::pair<std::string_view, ParameterValue>> values)
        {
            const uint32_t offset = static_cast<uint32_t>(parameters.size());
            for (auto &[name, value] : values)
                parameters.push_back(Parameter{model.intern(name), std::move(value)});
            steps.push_back(InitStep{model.intern(kind),
                                     Range{offset, static_cast<uint32_t>(parameters.size() - offset)}});
        }

        void addStorageInit(GrhSimModel &model, const Operation &declaration, StateId state,
                            LogicDomain domain)
        {
            std::vector<InitStep> steps;
            std::vector<Parameter> parameters;
            const std::string defaultValue = domain == LogicDomain::TwoState ? "0" : "x";
            const bool isArrayRegister =
                declaration.kind() == OperationKind::kRegister &&
                declaration.attr("shape").has_value();
            if (!isArrayRegister)
            {
                const std::string init = attr<std::string>(declaration, "initValue").value_or(defaultValue);
                if (init == "$random")
                    appendInitStep(model, steps, parameters, "core.init.random", {});
                else
                    appendInitStep(model, steps, parameters, "core.init.const",
                                   {{"value", ParameterValue(init)}});
                model.addInit(state, steps, parameters);
                return;
            }

            appendInitStep(model, steps, parameters, "core.init.fill",
                           {{"value", ParameterValue(defaultValue)}});
            const auto kinds = attr<std::vector<std::string>>(declaration, "initKind").value_or(
                std::vector<std::string>{});
            const auto files = attr<std::vector<std::string>>(declaration, "initFile").value_or(
                std::vector<std::string>{});
            const auto values = attr<std::vector<std::string>>(declaration, "initValue").value_or(
                std::vector<std::string>{});
            const auto starts = attr<std::vector<int64_t>>(declaration, "initStart").value_or(
                std::vector<int64_t>{});
            const auto lengths = attr<std::vector<int64_t>>(declaration, "initLen").value_or(
                std::vector<int64_t>{});
            for (std::size_t i = 0; i < kinds.size(); ++i)
            {
                std::vector<std::pair<std::string_view, ParameterValue>> stepParameters;
                if (kinds[i] == "readmemh" || kinds[i] == "readmemb")
                {
                    stepParameters.emplace_back("file", i < files.size() ? files[i] : std::string());
                    stepParameters.emplace_back("format", std::string(kinds[i] == "readmemh" ? "hex" : "bin"));
                    if (i < starts.size() && starts[i] >= 0)
                        stepParameters.emplace_back("start", starts[i]);
                    if (i < lengths.size() && lengths[i] > 0)
                        stepParameters.emplace_back("count", lengths[i]);
                    appendInitStep(model, steps, parameters, "core.init.readmem", std::move(stepParameters));
                }
                else if (kinds[i] == "literal")
                {
                    const std::string value = i < values.size() && !values[i].empty()
                                                  ? values[i] : defaultValue;
                    if (value == "$random")
                        stepParameters.emplace_back("random", true);
                    else
                        stepParameters.emplace_back("value", value);
                    if (i < starts.size() && starts[i] >= 0)
                        stepParameters.emplace_back("start", starts[i]);
                    if (i < lengths.size() && lengths[i] > 0)
                        stepParameters.emplace_back("count", lengths[i]);
                    appendInitStep(model, steps, parameters, "core.init.fill", std::move(stepParameters));
                }
            }
            model.addInit(state, steps, parameters);
        }
    } // namespace

    std::unique_ptr<GrhSimModel> lowerGrhToGrhSim(
        const Design &design, const GrhToGrhSimOptions &options,
        diag::Diagnostics &diagnostics)
    {
        const Graph *graph = selectTop(design, options, diagnostics);
        if (!graph) return nullptr;

        for (grh::OperationId opId : graph->operations())
        {
            const OperationKind kind = graph->opKind(opId);
            if (isForbiddenHierarchy(kind))
            {
                diagnostics.error("flat GRH prerequisite violated by " +
                                  std::string(toString(kind)), std::string(graph->symbol()));
            }
        }
        if (diagnostics.hasError()) return nullptr;

        auto model = std::make_unique<GrhSimModel>(graph->symbol());
        model->addDialect("core", "1", "wolvrix.grhsim.core.v1");

        std::size_t operandCount = 0;
        std::size_t resultCount = 0;
        std::size_t parameterCount = 0;
        uint32_t maxValueIndex = 0;
        for (grh::ValueId id : graph->values()) maxValueIndex = std::max(maxValueIndex, id.index);
        for (grh::OperationId id : graph->operations())
        {
            operandCount += graph->opOperands(id).size();
            resultCount += graph->opResults(id).size();
            parameterCount += graph->opAttrs(id).size();
        }
        model->reserve(ModelReserve{
            .strings = (options.keepOrigins
                            ? graph->values().size() + graph->operations().size()
                            : graph->operations().size() / 8 + 128) +
                       (options.keepDeclaredSymbols ? graph->declaredSymbols().size() : 0),
            .dialects = 1,
            .types = 32,
            .inputs = graph->inputPorts().size() + graph->inoutPorts().size(),
            .outputs = graph->outputPorts().size() + graph->inoutPorts().size() * 2,
            .states = graph->operations().size() / 8,
            .functions = 64,
            .functionArguments = 256,
            .interfacePorts = graph->inputPorts().size() + graph->outputPorts().size() +
                              graph->inoutPorts().size(),
            .values = graph->values().size(),
            .operations = graph->operations().size() + graph->inputPorts().size() +
                          graph->outputPorts().size() + graph->inoutPorts().size() * 3,
            .operands = operandCount + graph->outputPorts().size() + graph->inoutPorts().size() * 2,
            .results = resultCount + graph->inputPorts().size() + graph->inoutPorts().size(),
            .objectRefs = graph->operations().size() / 8,
            .parameters = parameterCount,
            .initRecords = graph->operations().size() / 8,
            .initSteps = graph->operations().size() / 8,
            .initParameters = graph->operations().size() / 8,
            .origins = options.keepOrigins ? graph->values().size() + graph->operations().size() : 0,
            .declaredSymbols = options.keepDeclaredSymbols ? graph->declaredSymbols().size() : 0,
            .generateGroups = options.keepDeclaredSymbols ? graph->generateGroups().size() : 0,
            .declProvenances = options.keepDeclaredSymbols ? graph->declaredSymbols().size() : 0});

        std::vector<grhsim::ValueId> valueMap(static_cast<std::size_t>(maxValueIndex) + 1);
        for (grh::ValueId valueId : graph->values())
        {
            const auto value = graph->getValue(valueId);
            // GRH rewrites can leave detached names after rebinding output ports.
            if (!value.definingOp().valid() && !value.isInput() && !value.isOutput() &&
                !value.isInout() && value.users().empty())
                continue;
            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.value",
                                               value.symbolText(), valueId.index, value.srcLoc());
            valueMap[valueId.index] = model->addValue(
                valueType(*model, *graph, valueId, options.logicDomain),
                options.keepOrigins ? value.symbolText() : std::string_view{}, origin);
            if (value.definingOp().valid() || value.isInput()) continue;
            const auto inoutPorts = graph->inoutPorts();
            if (std::any_of(inoutPorts.begin(), inoutPorts.end(),
                            [valueId](const InoutPort &port) { return port.in == valueId; }))
                continue;
            if (options.logicDomain != LogicDomain::TwoState || value.type() != ValueType::Logic)
            {
                diagnostics.error("undriven GRH value requires 2-state logic lowering",
                                  std::string(value.symbolText()));
                continue;
            }
            // Match legacy zero-initialized storage for referenced undriven logic.
            const std::array results{valueMap[valueId.index]};
            const std::array parameters{Parameter{model->intern("constValue"),
                std::to_string(std::max<int32_t>(1, value.width())) + "'h0"}};
            model->addOperation("core.compute.constant", {}, results, {}, parameters,
                                options.keepOrigins ? value.symbolText() : std::string_view{}, origin);
        }

        auto mappedValue = [&](grh::ValueId value, std::string_view context) -> grhsim::ValueId {
            if (!value.valid() || value.index >= valueMap.size() || !valueMap[value.index].valid())
            {
                diagnostics.error("GRH value reference is not present in the selected graph",
                                  std::string(context));
                return {};
            }
            return valueMap[value.index];
        };

        // stage-03: GRH whole writes (kRegisterWritePort) and unmasked array
        // writes (kRegisterWriteArrayPort) drop the constant full mask, while
        // grhsim regWrite/memWrite keep [en, (idx...), data, mask, events...].
        // Synthesize (cached) all-ones constant values for those masks.
        std::unordered_map<uint32_t, grhsim::ValueId> allOnesMaskByWidth;
        auto allOnesMaskFor = [&](uint32_t width) -> grhsim::ValueId {
            const uint32_t normalized = std::max<uint32_t>(1, width);
            auto [it, inserted] = allOnesMaskByWidth.try_emplace(normalized);
            if (!inserted)
                return it->second;
            const grhsim::ValueId value = model->addValue(
                model->logicType(normalized, false, options.logicDomain), {}, {});
            std::string literal = std::to_string(normalized) + "'b";
            literal.append(normalized, '1');
            const std::array results{value};
            const std::array parameters{Parameter{model->intern("constValue"), std::move(literal)}};
            model->addOperation("core.compute.constant", {}, results, {}, parameters, {}, {});
            it->second = value;
            return value;
        };

        std::unordered_map<int64_t, grhsim::ValueId> constValueCache;
        auto constValueFor = [&](int64_t literal) -> grhsim::ValueId {
            auto [it, inserted] = constValueCache.try_emplace(literal);
            if (!inserted)
                return it->second;
            const grhsim::ValueId value = model->addValue(
                model->logicType(32, false, options.logicDomain), {}, {});
            const std::array results{value};
            const std::array parameters{
                Parameter{model->intern("constValue"), std::to_string(literal)}};
            model->addOperation("core.compute.constant", {}, results, {}, parameters, {}, {});
            it->second = value;
            return value;
        };
        auto addComputeOp = [&](std::string_view name, std::vector<grhsim::ValueId> inputs,
                                int64_t resultWidth, bool isSigned,
                                std::vector<Parameter> parameters = {}) -> grhsim::ValueId {
            const grhsim::ValueId result = model->addValue(
                model->logicType(static_cast<uint32_t>(std::max<int64_t>(1, resultWidth)),
                                 isSigned, options.logicDomain),
                {}, {});
            const std::array results{result};
            model->addOperation(name, inputs, results, {}, parameters, {}, {});
            return result;
        };
        // stage-04 transition: flat = sum(idx_j * stride_j), stride_j =
        // product(shape[j+1:]) (row-major, dim 0 outermost).
        auto linearizedIndex = [&](std::span<const grh::ValueId> grhIndices,
                                   const std::vector<int64_t> &shape, auto &&mapFn,
                                   std::string_view context) -> grhsim::ValueId {
            grhsim::ValueId flat;
            for (std::size_t i = 0; i < grhIndices.size(); ++i)
            {
                int64_t stride = 1;
                for (std::size_t j = i + 1; j < shape.size(); ++j)
                    stride *= std::max<int64_t>(1, shape[j]);
                grhsim::ValueId term = mapFn(grhIndices[i], context);
                if (!term.valid())
                    return {};
                if (stride != 1)
                    term = addComputeOp("core.compute.mul", {term, constValueFor(stride)}, 32, false);
                flat = flat.valid() ? addComputeOp("core.compute.add", {flat, term}, 32, false)
                                    : term;
            }
            return flat;
        };
        auto sliceElement = [&](grhsim::ValueId base, int64_t elemWidth, int64_t index,
                                bool isSigned) -> grhsim::ValueId {
            const int64_t lo = index * elemWidth;
            const int64_t hi = lo + elemWidth - 1;
            return addComputeOp("core.compute.sliceStatic", {base}, elemWidth, isSigned,
                                {Parameter{model->intern("sliceStart"), lo},
                                 Parameter{model->intern("sliceEnd"), hi}});
        };

        struct ArrayStateInfo {
            std::vector<int64_t> shape;
            int64_t elementWidth = 1;
            bool isSigned = false;
        };
        std::unordered_map<std::string, ArrayStateInfo> arrayShapeBySymbol;
        std::unordered_map<std::string, StateId> statesBySymbol;
        std::unordered_set<std::string> arrayRegisterSymbols;
        std::unordered_map<std::string, ValueType> storageReadKinds;
        for (grh::OperationId opId : graph->operations())
        {
            std::string_view key;
            const OperationKind kind = graph->opKind(opId);
            if (kind == OperationKind::kRegisterReadPort) key = "regSymbol";
            else if (kind == OperationKind::kLatchReadPort) key = "latchSymbol";
            else continue;
            const Operation op = graph->getOperation(opId);
            const auto symbol = attr<std::string>(op, key);
            if (symbol && !op.results().empty())
                storageReadKinds.insert_or_assign(*symbol, graph->valueType(op.results().front()));
        }
        std::vector<std::pair<grh::OperationId, StateId>> storageDeclarations;
        for (grh::OperationId opId : graph->operations())
        {
            const Operation op = graph->getOperation(opId);
            if (op.kind() != OperationKind::kRegister && op.kind() != OperationKind::kLatch)
                continue;
            const std::string symbol(op.symbolText());
            if (symbol.empty() || statesBySymbol.contains(symbol))
            {
                diagnostics.error("storage declaration has an empty or duplicate symbol", symbol);
                continue;
            }
            TypeId type;
            const int64_t width = attr<int64_t>(op, "width").value_or(0);
            const bool isSigned = attr<bool>(op, "isSigned").value_or(false);
            const auto shape = attr<std::vector<int64_t>>(op, "shape").value_or(std::vector<int64_t>{});
            if (op.kind() == OperationKind::kRegister && !shape.empty())
            {
                // stage-04 transition: an array register maps to one FLAT
                // core.array state of product(shape) elements; multi-dim
                // index tuples are linearized at this boundary (row-major)
                // until the stage-06 backend rework natively supports
                // multi-dim states. `width` is the total bit width.
                int64_t elementCount = 1;
                for (const int64_t dim : shape)
                {
                    if (dim <= 0)
                    {
                        diagnostics.error("array register declaration has a non-positive dimension", symbol);
                        elementCount = 0;
                        break;
                    }
                    elementCount *= dim;
                }
                if (elementCount <= 0 || width <= 0 || width % elementCount != 0)
                {
                    diagnostics.error("array register declaration has invalid width or shape", symbol);
                    continue;
                }
                const int64_t elementWidth = width / elementCount;
                TypeId element = model->logicType(static_cast<uint32_t>(elementWidth),
                                                  isSigned, options.logicDomain);
                type = model->arrayType(element, static_cast<uint64_t>(elementCount));
                arrayShapeBySymbol.emplace(symbol, ArrayStateInfo{shape, elementWidth, isSigned});
            }
            else
            {
                const auto kindIt = storageReadKinds.find(symbol);
                const ValueType storageKind = kindIt == storageReadKinds.end()
                                                      ? ValueType::Logic : kindIt->second;
                type = scalarType(*model, storageKind,
                                  width, isSigned, options.logicDomain);
            }
            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.operation",
                                               symbol, opId.index, op.srcLoc());
            const StateId state = model->addState(symbol, type, origin);
            statesBySymbol.emplace(symbol, state);
            if (op.kind() == OperationKind::kRegister && !shape.empty())
                arrayRegisterSymbols.insert(symbol);
            storageDeclarations.emplace_back(opId, state);
        }

        std::unordered_map<std::string, FuncId> functionsBySymbol;
        for (grh::OperationId opId : graph->operations())
        {
            const Operation op = graph->getOperation(opId);
            if (op.kind() != OperationKind::kDpicImport) continue;
            const std::string symbol(op.symbolText());
            if (symbol.empty() || functionsBySymbol.contains(symbol))
            {
                diagnostics.error("DPI import has an empty or duplicate symbol", symbol);
                continue;
            }
            const auto directions = attr<std::vector<std::string>>(op, "argsDirection").value_or(
                std::vector<std::string>{});
            const auto widths = attr<std::vector<int64_t>>(op, "argsWidth").value_or(
                std::vector<int64_t>{});
            const auto names = attr<std::vector<std::string>>(op, "argsName").value_or(
                std::vector<std::string>{});
            const auto signedness = attr<std::vector<bool>>(op, "argsSigned").value_or(
                std::vector<bool>{});
            const auto typeNames = attr<std::vector<std::string>>(op, "argsType").value_or(
                std::vector<std::string>{});
            if (directions.size() != widths.size() || directions.size() != names.size() ||
                directions.size() != signedness.size())
            {
                diagnostics.error("DPI import argument metadata arrays have different sizes", symbol);
                continue;
            }
            std::vector<DpiArgument> arguments;
            arguments.reserve(directions.size());
            bool valid = true;
            for (std::size_t i = 0; i < directions.size(); ++i)
            {
                auto direction = parseDpiDirection(directions[i]);
                if (!direction)
                {
                    diagnostics.error("DPI import has invalid argument direction: " + directions[i], symbol);
                    valid = false;
                    break;
                }
                const ValueType valueKind = i < typeNames.size()
                                                ? parseDpiValueType(typeNames[i]) : ValueType::Logic;
                arguments.push_back(DpiArgument{
                    model->intern(names[i]), *direction,
                    scalarType(*model, valueKind, widths[i], signedness[i], options.logicDomain)});
            }
            if (!valid) continue;
            TypeId returnType;
            if (attr<bool>(op, "hasReturn").value_or(false))
            {
                returnType = scalarType(
                    *model,
                    parseDpiValueType(attr<std::string>(op, "returnType").value_or("logic")),
                    attr<int64_t>(op, "returnWidth").value_or(1),
                    attr<bool>(op, "returnSigned").value_or(false), options.logicDomain);
            }
            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.operation",
                                               symbol, opId.index, op.srcLoc());
            const FuncId function = model->addExternFunction(symbol, "core.dpi", symbol,
                                                              arguments, returnType, origin);
            functionsBySymbol.emplace(symbol, function);
        }

        if (diagnostics.hasError()) return nullptr;
        for (const auto &[declarationId, state] : storageDeclarations)
            addStorageInit(*model, graph->getOperation(declarationId), state, options.logicDomain);

        for (const Port &port : graph->inputPorts())
        {
            const grhsim::ValueId value = mappedValue(port.value, port.name);
            const auto grhValue = graph->getValue(port.value);
            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.port",
                                               port.name, port.value.index, grhValue.srcLoc());
            const InputId input = model->addInput(port.name, valueType(*model, *graph, port.value,
                                                                       options.logicDomain), origin);
            model->addInterfacePort(InterfacePort{model->intern(port.name), InterfaceDirection::Input,
                                                  input, {}, {}});
            const std::array<ObjectRef, 1> refs{ObjectRef::input(input)};
            const std::array<grhsim::ValueId, 1> results{value};
            model->addOperation("core.input.read", {}, results, refs, {}, port.name, origin);
        }
        for (const InoutPort &port : graph->inoutPorts())
        {
            const grhsim::ValueId inValue = mappedValue(port.in, port.name);
            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.port",
                                               port.name, port.in.index, graph->valueSrcLoc(port.in));
            const InputId input = model->addInput(port.name + "$in",
                                                  valueType(*model, *graph, port.in, options.logicDomain), origin);
            const OutputId output = model->addOutput(port.name + "$out",
                                                     valueType(*model, *graph, port.out, options.logicDomain), origin);
            const OutputId outputEnable = model->addOutput(
                port.name + "$oe", valueType(*model, *graph, port.oe, options.logicDomain), origin);
            model->addInterfacePort(InterfacePort{model->intern(port.name), InterfaceDirection::Inout,
                                                  input, output, outputEnable});
            const std::array<ObjectRef, 1> inputRef{ObjectRef::input(input)};
            const std::array<grhsim::ValueId, 1> inputResult{inValue};
            model->addOperation("core.input.read", {}, inputResult, inputRef, {}, port.name, origin);
        }

        auto stateFor = [&](const Operation &op, std::string_view key) -> StateId {
            const auto symbol = attr<std::string>(op, key);
            if (!symbol)
            {
                diagnostics.error("storage port is missing target attribute " + std::string(key),
                                  std::string(op.symbolText()));
                return {};
            }
            auto it = statesBySymbol.find(*symbol);
            if (it == statesBySymbol.end())
            {
                diagnostics.error("storage port target does not resolve: " + *symbol,
                                  std::string(op.symbolText()));
                return {};
            }
            return it->second;
        };

        for (grh::OperationId opId : graph->operations())
        {
            const Operation op = graph->getOperation(opId);
            if (isDeclaration(op.kind())) continue;

            // ---- stage-04 transition: array-state ports on flat states ----
            const auto arrayTargetInfo = [&]() ->
                std::optional<std::pair<ArrayStateInfo, StateId>> {
                const auto symbol = attr<std::string>(op, "regSymbol");
                if (!symbol) return std::nullopt;
                auto infoIt = arrayShapeBySymbol.find(*symbol);
                if (infoIt == arrayShapeBySymbol.end()) return std::nullopt;
                auto stateIt = statesBySymbol.find(*symbol);
                if (stateIt == statesBySymbol.end()) return std::nullopt;
                return std::pair{infoIt->second, stateIt->second};
            }();

            // Whole-array read (kRegisterReadPort on an array register):
            // unroll to per-element memReads + row-major concat.
            if (op.kind() == OperationKind::kRegisterReadPort && arrayTargetInfo &&
                !op.results().empty())
            {
                const auto &[info, state] = *arrayTargetInfo;
                int64_t total = 1;
                for (const int64_t dim : info.shape) total *= std::max<int64_t>(1, dim);
                std::vector<grhsim::ValueId> elems;
                elems.reserve(static_cast<std::size_t>(total));
                for (int64_t j = 0; j < total; ++j)
                {
                    grhsim::ValueId elem = model->addValue(
                        model->logicType(static_cast<uint32_t>(info.elementWidth),
                                         info.isSigned, options.logicDomain),
                        {}, {});
                    const std::array results{elem};
                    const std::array refs{ObjectRef::state(state)};
                    const std::array operands{constValueFor(j)};
                    model->addOperation("core.state.memRead", operands, results, refs,
                                        {}, {}, {});
                    elems.push_back(elem);
                }
                const grhsim::ValueId result = mappedValue(op.results().front(), op.symbolText());
                if (!result.valid()) continue;
                if (elems.size() == 1)
                {
                    const std::array operands{elems.front()};
                    const std::array results{result};
                    model->addOperation("core.compute.assign", operands, results, {}, {}, {}, {});
                }
                else
                {
                    std::reverse(elems.begin(), elems.end());
                    const std::array results{result};
                    model->addOperation("core.compute.concat", elems, results, {}, {}, {}, {});
                }
                continue;
            }

            // Array read port with 1..rank indices: linearize the index
            // tuple; fewer than rank indices unroll the addressed sub-array.
            if (op.kind() == OperationKind::kRegisterReadArrayPort && arrayTargetInfo &&
                !op.results().empty())
            {
                const auto &[info, state] = *arrayTargetInfo;
                const std::size_t rank = info.shape.size();
                std::vector<grh::ValueId> grhIndices(op.operands().begin(), op.operands().end());
                if (grhIndices.empty() || grhIndices.size() > rank)
                {
                    diagnostics.error("array read port index count out of range",
                                      std::string(op.symbolText()));
                    continue;
                }
                grhsim::ValueId flat =
                    linearizedIndex(grhIndices, info.shape, mappedValue, op.symbolText());
                if (!flat.valid()) continue;
                const grhsim::ValueId result = mappedValue(op.results().front(), op.symbolText());
                if (!result.valid()) continue;
                if (grhIndices.size() == rank)
                {
                    const std::array operands{flat};
                    const std::array results{result};
                    const std::array refs{ObjectRef::state(state)};
                    model->addOperation("core.state.memRead", operands, results, refs, {}, {}, {});
                    continue;
                }
                int64_t sub = 1;
                for (std::size_t i = grhIndices.size(); i < rank; ++i)
                    sub *= std::max<int64_t>(1, info.shape[i]);
                std::vector<grhsim::ValueId> elems;
                elems.reserve(static_cast<std::size_t>(sub));
                for (int64_t j = 0; j < sub; ++j)
                {
                    grhsim::ValueId addr =
                        j == 0 ? flat
                               : addComputeOp("core.compute.add", {flat, constValueFor(j)}, 32, false);
                    grhsim::ValueId elem = model->addValue(
                        model->logicType(static_cast<uint32_t>(info.elementWidth),
                                         info.isSigned, options.logicDomain),
                        {}, {});
                    const std::array results{elem};
                    const std::array refs{ObjectRef::state(state)};
                    const std::array operands{addr};
                    model->addOperation("core.state.memRead", operands, results, refs, {}, {}, {});
                    elems.push_back(elem);
                }
                std::reverse(elems.begin(), elems.end());
                const std::array results{result};
                model->addOperation("core.compute.concat", elems, results, {}, {}, {}, {});
                continue;
            }

            // Array write port with fewer than rank indices: unroll the
            // addressed sub-array into per-element memWrites with sliced
            // data/mask. Full-rank writes fall through to the generic path
            // (single linearized memWrite is handled there too — but the
            // generic path passes the raw indices, so intercept all array
            // write ports here).
            if ((op.kind() == OperationKind::kRegisterWriteArrayPort ||
                 op.kind() == OperationKind::kRegisterWriteArrayMaskPort) &&
                arrayTargetInfo)
            {
                const auto &[info, state] = *arrayTargetInfo;
                const std::size_t rank = info.shape.size();
                const std::optional<std::size_t> eventStart = grh::firstEventOperandIndex(op);
                const bool hasMask =
                    op.kind() == OperationKind::kRegisterWriteArrayMaskPort;
                const std::size_t fixedCount = 2 + (hasMask ? 1 : 0);  // cond+data(+mask)
                if (!eventStart || *eventStart < fixedCount + 1)
                {
                    diagnostics.error("array write port layout does not locate event operands",
                                      std::string(op.symbolText()));
                    continue;
                }
                const std::size_t indexCount = *eventStart - fixedCount;
                if (indexCount < 1 || indexCount > rank)
                {
                    diagnostics.error("array write port index count out of range",
                                      std::string(op.symbolText()));
                    continue;
                }
                const grh::ValueId grhCond = op.operands()[0];
                const grh::ValueId grhData = op.operands()[1 + indexCount];
                std::vector<grh::ValueId> grhIndices(
                    op.operands().begin() + 1, op.operands().begin() + 1 + indexCount);
                grhsim::ValueId flat =
                    linearizedIndex(grhIndices, info.shape, mappedValue, op.symbolText());
                grhsim::ValueId cond = mappedValue(grhCond, op.symbolText());
                grhsim::ValueId data = mappedValue(grhData, op.symbolText());
                if (!flat.valid() || !cond.valid() || !data.valid()) continue;
                grhsim::ValueId mask;
                if (hasMask)
                {
                    mask = mappedValue(op.operands()[*eventStart - 1], op.symbolText());
                    if (!mask.valid()) continue;
                }
                std::vector<grhsim::ValueId> events;
                bool eventsOk = true;
                for (std::size_t i = *eventStart; i < op.operands().size(); ++i)
                {
                    grhsim::ValueId evt = mappedValue(op.operands()[i], op.symbolText());
                    if (!evt.valid()) { eventsOk = false; break; }
                    events.push_back(evt);
                }
                if (!eventsOk) continue;
                const auto edges = attr<std::vector<std::string>>(op, "eventEdge").value_or(
                    std::vector<std::string>{});
                std::vector<Parameter> writeParams;
                if (!edges.empty())
                    writeParams.push_back(Parameter{model->intern("event_edges"),
                                                    ParameterValue(edges)});
                int64_t sub = 1;
                for (std::size_t i = indexCount; i < rank; ++i)
                    sub *= std::max<int64_t>(1, info.shape[i]);
                for (int64_t j = 0; j < sub; ++j)
                {
                    grhsim::ValueId addr =
                        j == 0 ? flat
                               : addComputeOp("core.compute.add", {flat, constValueFor(j)}, 32, false);
                    grhsim::ValueId dataJ =
                        sub == 1 ? data
                                 : sliceElement(data, info.elementWidth, j, info.isSigned);
                    grhsim::ValueId maskJ =
                        hasMask ? (sub == 1 ? mask
                                            : sliceElement(mask, info.elementWidth, j, info.isSigned))
                                : allOnesMaskFor(static_cast<uint32_t>(info.elementWidth));
                    if (!dataJ.valid() || !maskJ.valid()) break;
                    std::vector<grhsim::ValueId> operands;
                    operands.reserve(4 + events.size());
                    operands.push_back(cond);
                    operands.push_back(addr);
                    operands.push_back(dataJ);
                    operands.push_back(maskJ);
                    operands.insert(operands.end(), events.begin(), events.end());
                    const std::array refs{ObjectRef::state(state)};
                    model->addOperation("core.state.memWrite", operands, {}, refs, writeParams,
                                        {}, {});
                }
                continue;
            }

            // stage-04: a whole write (kRegisterWritePort) targeting an array
            // register is the packed full-width fill form — lower to memFill
            // (which slices the packed value row-major) instead of regWrite.
            const bool isArrayWholeWrite =
                op.kind() == OperationKind::kRegisterWritePort &&
                [&] {
                    const auto symbol = attr<std::string>(op, "regSymbol");
                    return symbol && arrayRegisterSymbols.contains(*symbol);
                }();
            const std::optional<std::string_view> opName =
                isArrayWholeWrite ? std::optional<std::string_view>{"core.state.memFill"}
                                  : loweredOpName(op.kind());
            if (!opName)
            {
                diagnostics.error("GRH operation has no core lowering: " +
                                  std::string(toString(op.kind())), std::string(op.symbolText()));
                continue;
            }
            std::vector<grhsim::ValueId> operands;
            operands.reserve(op.operands().size() + 1);
            for (grh::ValueId value : op.operands()) operands.push_back(mappedValue(value, op.symbolText()));
            if ((op.kind() == OperationKind::kRegisterWritePort && !isArrayWholeWrite) ||
                op.kind() == OperationKind::kRegisterWriteArrayPort)
            {
                // Insert the synthesized full mask right before the events.
                const std::optional<std::size_t> eventStart = grh::firstEventOperandIndex(op);
                if (!eventStart || *eventStart < 2 || *eventStart > operands.size())
                {
                    diagnostics.error("write port layout does not locate event operands",
                                      std::string(op.symbolText()));
                    continue;
                }
                const uint32_t dataWidth = static_cast<uint32_t>(std::max<int32_t>(
                    1, graph->valueWidth(op.operands()[*eventStart - 1])));
                operands.insert(operands.begin() + static_cast<std::ptrdiff_t>(*eventStart),
                                allOnesMaskFor(dataWidth));
            }
            std::vector<grhsim::ValueId> results;
            results.reserve(op.results().size());
            for (grh::ValueId value : op.results()) results.push_back(mappedValue(value, op.symbolText()));

            std::vector<ObjectRef> refs;
            const std::string_view targetKey = targetAttribute(op.kind());
            if (!targetKey.empty())
            {
                if (op.kind() == OperationKind::kDpicCall)
                {
                    const auto symbol = attr<std::string>(op, targetKey);
                    auto it = symbol ? functionsBySymbol.find(*symbol) : functionsBySymbol.end();
                    if (it == functionsBySymbol.end())
                        diagnostics.error("DPI call target does not resolve", std::string(op.symbolText()));
                    else
                        refs.push_back(ObjectRef::function(it->second));
                }
                else
                {
                    const StateId state = stateFor(op, targetKey);
                    if (state.valid()) refs.push_back(ObjectRef::state(state));
                }
            }

            std::vector<Parameter> parameters;
            parameters.reserve(op.attrs().size());
            for (const AttrKV &entry : op.attrs())
            {
                if (entry.key == targetKey) continue;
                parameters.push_back(Parameter{model->intern(normalizedParameterName(entry.key)),
                                               copyAttribute(entry.value)});
            }
            if (op.kind() == OperationKind::kSliceArray)
            {
                // stage-02: GRH kSliceArray no longer carries sliceWidth; the
                // grhsim core.compute.sliceArray op keeps its sliceWidth
                // parameter (= element width) until the stage-06 multi-index
                // rework. Only single-index form is supported here.
                if (operands.size() != 2 || results.empty())
                {
                    diagnostics.error("kSliceArray with != 1 index operands is not supported by the "
                                      "grhsim lowering yet (stage-06)",
                                      std::string(op.symbolText()));
                    continue;
                }
                parameters.push_back(Parameter{
                    model->intern("sliceWidth"),
                    static_cast<int64_t>(graph->valueWidth(op.results().front()))});
            }

            // Event-sensitive ops carry the raw event annotation for
            // grhsim.lower-edge-detect: the trailing len(event_edges) operands
            // are the event values. No __event_* history states are
            // materialized (removed in M5; the pass derives prevInit itself).
            if (isEventSensitive(op.kind()))
            {
                const auto edges = attr<std::vector<std::string>>(op, "eventEdge").value_or(
                    std::vector<std::string>{});
                if (edges.size() > operands.size())
                {
                    diagnostics.error("event edge count exceeds operation operand count",
                                      std::string(op.symbolText()));
                }
            }

            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.operation",
                                               op.symbolText(), opId.index, op.srcLoc());
            model->addOperation(*opName, operands, results, refs, parameters,
                                options.keepOrigins ? op.symbolText() : std::string_view{}, origin);
        }

        for (const Port &port : graph->outputPorts())
        {
            const grhsim::ValueId value = mappedValue(port.value, port.name);
            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.port",
                                               port.name, port.value.index, graph->valueSrcLoc(port.value));
            const OutputId output = model->addOutput(port.name,
                                                     valueType(*model, *graph, port.value,
                                                               options.logicDomain), origin);
            model->addInterfacePort(InterfacePort{model->intern(port.name), InterfaceDirection::Output,
                                                  {}, output, {}});
            const std::array<grhsim::ValueId, 1> operands{value};
            const std::array<ObjectRef, 1> refs{ObjectRef::output(output)};
            model->addOperation("core.output.write", operands, {}, refs, {}, port.name, origin);
        }
        for (const InoutPort &port : graph->inoutPorts())
        {
            auto interfaceIt = std::find_if(model->interfacePorts().begin(), model->interfacePorts().end(),
                                            [&](const InterfacePort &entry) {
                                                return entry.direction == InterfaceDirection::Inout &&
                                                       model->text(entry.name) == port.name;
                                            });
            if (interfaceIt == model->interfacePorts().end()) continue;
            const OriginId origin = makeOrigin(*model, options.keepOrigins, "grh.port",
                                               port.name, port.out.index, graph->valueSrcLoc(port.out));
            const std::array<grhsim::ValueId, 1> outOperand{mappedValue(port.out, port.name)};
            const std::array<ObjectRef, 1> outRef{ObjectRef::output(interfaceIt->output)};
            model->addOperation("core.output.write", outOperand, {}, outRef, {}, port.name + "$out", origin);
            const std::array<grhsim::ValueId, 1> oeOperand{mappedValue(port.oe, port.name)};
            const std::array<ObjectRef, 1> oeRef{ObjectRef::output(interfaceIt->outputEnable)};
            model->addOperation("core.output.write", oeOperand, {}, oeRef, {}, port.name + "$oe", origin);
        }

        if (diagnostics.hasError()) return nullptr;

        // Read-only provenance metadata: carry the (flattened) declared symbol
        // names and generate copy groups as pure text. Empty names are skipped;
        // the model deduplicates.
        if (options.keepDeclaredSymbols)
        {
            for (const grh::SymbolId symbol : graph->declaredSymbols())
            {
                const std::string_view text = graph->symbolText(symbol);
                if (text.empty()) continue;
                model->addDeclaredSymbol(model->intern(text));
            }
            for (const grh::Graph::GenerateGroup &group : graph->generateGroups())
            {
                const std::string_view scope = graph->symbolText(group.scope);
                const std::string_view name = graph->symbolText(group.name);
                if (scope.empty() || name.empty()) continue;
                const std::size_t index =
                    model->addGenerateGroup(model->intern(scope), model->intern(name));
                for (const grh::SymbolId symbol : group.symbols)
                {
                    const std::string_view text = graph->symbolText(symbol);
                    if (text.empty()) continue;
                    model->addGenerateGroupSymbol(index, model->intern(text));
                }
            }

            // Maintainable declaration provenance: resolve every declared
            // symbol to the entity the lowering just created for it and record
            // one Direct full-range slice. Symbols that do not resolve to a
            // live value/state/function stay as bare anchors in declaredSymbols
            // (see DeclProvenance's contract).
            const auto declarationShape = [&](TypeId type, std::vector<uint64_t> &shape) -> uint64_t {
                const Type *node = &model->types()[static_cast<std::size_t>(type.index) - 1];
                while (node->kind == TypeKind::Array)
                {
                    shape.push_back(node->count);
                    node = &model->types()[static_cast<std::size_t>(node->elementType.index) - 1];
                }
                return node->kind == TypeKind::Logic ? node->width : 0;
            };
            for (const grh::SymbolId symbol : graph->declaredSymbols())
            {
                const std::string_view text = graph->symbolText(symbol);
                if (text.empty()) continue;
                DeclProvenance record;
                record.symbol = model->intern(text);
                if (const grh::ValueId valueId = graph->findValue(symbol); valueId.valid())
                {
                    if (valueId.index >= valueMap.size() || !valueMap[valueId.index].valid())
                        continue; // detached declared value skipped by the value lowering
                    const grhsim::ValueId target = valueMap[valueId.index];
                    const SimValue &value = model->values()[static_cast<std::size_t>(target.index) - 1];
                    record.origin = value.origin;
                    record.width = declarationShape(value.type, record.shape);
                    record.slices.push_back(DeclProvenanceSlice{DeclProvenanceKind::Direct,
                                                                DeclProvenanceTarget::Value,
                                                                target.index, 0, 0, record.width});
                }
                else if (const grh::OperationId opId = graph->findOperation(symbol); opId.valid())
                {
                    const Operation op = graph->getOperation(opId);
                    if (op.kind() == OperationKind::kRegister || op.kind() == OperationKind::kLatch)
                    {
                        const auto stateIt = statesBySymbol.find(std::string(text));
                        if (stateIt == statesBySymbol.end()) continue;
                        const StateObject &state =
                            model->states()[static_cast<std::size_t>(stateIt->second.index) - 1];
                        record.origin = state.origin;
                        record.width = declarationShape(state.type, record.shape);
                        uint64_t linearWidth = record.width;
                        for (const uint64_t dim : record.shape) linearWidth *= dim;
                        record.slices.push_back(DeclProvenanceSlice{DeclProvenanceKind::Direct,
                                                                    DeclProvenanceTarget::State,
                                                                    stateIt->second.index, 0, 0,
                                                                    linearWidth});
                    }
                    else if (op.kind() == OperationKind::kDpicImport)
                    {
                        const auto funcIt = functionsBySymbol.find(std::string(text));
                        if (funcIt == functionsBySymbol.end()) continue;
                        record.origin =
                            model->functions()[static_cast<std::size_t>(funcIt->second.index) - 1].origin;
                        record.slices.push_back(DeclProvenanceSlice{DeclProvenanceKind::Direct,
                                                                    DeclProvenanceTarget::Function,
                                                                    funcIt->second.index, 0, 0, 0});
                    }
                    else
                    {
                        continue; // only storage and DPI import declarations carry provenance
                    }
                }
                else
                {
                    continue;
                }
                model->upsertDeclProvenance(std::move(record));
            }
        }

        if (!verifyGrhSimModel(*model, defaultDialectRegistry(), diagnostics)) return nullptr;
        return model;
    }

} // namespace wolvrix::lib::grhsim
