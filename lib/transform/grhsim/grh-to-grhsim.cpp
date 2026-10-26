#include "transform/grhsim/grh-to-grhsim.hpp"

#include "transform/grhsim/ops.hpp"
#include "transform/verify.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <charconv>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;

struct StateConversion {
    OperationId declaration;
    LogicalType type;
    InitSpec init;
};

struct EventInput {
    ValueId value;
    std::string edge;
    std::string previous;
    bool fallback = false;
};

struct OpConversion {
    OperationId operation;
    OperationKind kind;
    std::vector<EventInput> events;
    int64_t writerOrder = -1;
    std::vector<std::pair<std::size_t, LogicalType>> inputViews;
};

slang::SVInt parseBits(std::string_view literal, int32_t width, std::optional<bool> sign = std::nullopt)
{
    if (literal == "x" || literal == "X" || literal == "'x" || literal == "'X")
        return slang::SVInt::createFillX(width, sign.value_or(false));
    if (literal == "z" || literal == "Z" || literal == "?" || literal == "'z" || literal == "'Z")
        return slang::SVInt::createFillZ(width, sign.value_or(false));
    if (literal == "'1") return ~slang::SVInt(width, 0, sign.value_or(false));
    if (literal == "'0") return slang::SVInt(width, 0, sign.value_or(false));
    auto bits = slang::SVInt::fromString(literal);
    if (sign) bits.setSigned(*sign);
    return bits.resize(width);
}

std::string projectLiteral(std::string_view literal, int32_t width, bool sign, LogicDomain domain)
{
    auto bits = parseBits(literal, width, sign);
    if (domain == LogicDomain::TwoState) bits.flattenUnknowns();
    return bits.toString(slang::LiteralBase::Hex, true, bits.getBitWidth());
}

uint64_t mask(int32_t width)
{
    return width >= 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
}

// Keep the legacy edge-history initialization policy, including its fallback
// for unsupported cones, wide/shaped values and mismatched init literal widths.
class EventInitialValue {
public:
    EventInitialValue(const Graph& graph, const std::vector<StateConversion>& states,
                      LogicDomain domain) : graph_(graph), domain_(domain)
    {
        for (const auto& state : states) states_.emplace(state.declaration, &state);
        for (const auto& port : graph.inoutPorts()) inputs_.insert(port.in);
    }

    std::optional<uint64_t> eval(ValueId value, uint32_t depth = 0)
    {
        if (depth > 64) return std::nullopt;
        const auto type = graph_.valueLogicalType(value);
        if (type.kind != ValueType::Logic || !type.shape.empty() || type.width > 64)
            return std::nullopt;
        if (const auto it = memo_.find(value); it != memo_.end()) return it->second;
        const auto result = compute(value, depth);
        if (result) memo_.emplace(value, result);
        return result;
    }

private:
    std::optional<uint64_t> compute(ValueId value, uint32_t depth)
    {
        const auto width = graph_.valueWidth(value);
        if (graph_.valueIsInput(value) || inputs_.contains(value)) return 0;
        const auto producer = graph_.valueDef(value);
        if (!producer) return domain_ == LogicDomain::TwoState ? std::optional<uint64_t>{0} : std::nullopt;
        const auto kind = graph_.opKind(producer);
        const auto operands = graph_.opOperands(producer);
        if (kind == OperationKind::kConstant)
        {
            auto bits = parseBits(attribute<std::string>(graph_, producer, "constValue").value(), width);
            bits.flattenUnknowns();
            return bits.getRawPtr()[0] & mask(width);
        }
        if (kind == OperationKind::kRegisterReadPort || kind == OperationKind::kLatchReadPort)
        {
            const auto symbol = attribute<std::string>(graph_, producer,
                kind == OperationKind::kLatchReadPort ? "latchSymbol" : "regSymbol");
            if (!symbol) return std::nullopt;
            const auto it = states_.find(graph_.findOperation(*symbol));
            if (it == states_.end() || !it->second->type.shape.empty()) return std::nullopt;
            for (const auto& init : it->second->init)
            {
                if (init.kind != InitKind::Constant) continue;
                const auto quote = init.value.find('\'');
                if (quote != std::string::npos && quote != 0)
                {
                    int32_t declared = 0;
                    const auto parsed = std::from_chars(init.value.data(), init.value.data() + quote, declared);
                    if (parsed.ec != std::errc{} || declared != width) return std::nullopt;
                }
                auto bits = parseBits(init.value, width);
                bits.flattenUnknowns();
                return bits.getRawPtr()[0] & mask(width);
            }
            return std::nullopt;
        }
        if (kind == OperationKind::kConcat)
        {
            uint64_t result = 0;
            int32_t offset = 0;
            for (std::size_t i = operands.size(); i-- > 0;)
            {
                const auto part = eval(operands[i], depth + 1);
                const auto partWidth = graph_.valueWidth(operands[i]);
                if (!part || partWidth > 64 - offset) return std::nullopt;
                result |= (*part & mask(partWidth)) << offset;
                offset += partWidth;
            }
            return result & mask(width);
        }
        if (operands.empty() || operands.size() > 3) return std::nullopt;
        std::array<uint64_t, 3> args{};
        for (std::size_t i = 0; i < operands.size(); ++i)
        {
            const auto arg = eval(operands[i], depth + 1);
            if (!arg) return std::nullopt;
            args[i] = *arg;
        }
        if (kind == OperationKind::kNot && operands.size() == 1) return ~args[0] & mask(width);
        if (kind == OperationKind::kLogicNot && operands.size() == 1) return args[0] == 0;
        if (operands.size() == 2)
        {
            if (kind == OperationKind::kAnd) return (args[0] & args[1]) & mask(width);
            if (kind == OperationKind::kOr) return (args[0] | args[1]) & mask(width);
            if (kind == OperationKind::kXor) return (args[0] ^ args[1]) & mask(width);
            if (kind == OperationKind::kXnor) return ~(args[0] ^ args[1]) & mask(width);
            if (kind == OperationKind::kLogicAnd) return args[0] != 0 && args[1] != 0;
            if (kind == OperationKind::kLogicOr) return args[0] != 0 || args[1] != 0;
        }
        if (kind == OperationKind::kMux && operands.size() == 3)
            return (args[0] ? args[1] : args[2]) & mask(width);
        if (kind == kBitSelect && operands.size() == 3)
            return ((args[0] & args[1]) | (~args[0] & args[2])) & mask(width);
        if (kind == OperationKind::kSliceStatic && operands.size() == 1)
        {
            const auto start = attribute<int64_t>(graph_, producer, "sliceStart");
            const auto end = attribute<int64_t>(graph_, producer, "sliceEnd");
            if (start && end && *start >= 0 && *end >= *start && *end < 64)
                return (args[0] >> *start) & mask(static_cast<int32_t>(*end - *start + 1));
        }
        return std::nullopt;
    }

    const Graph& graph_;
    LogicDomain domain_;
    std::unordered_map<OperationId, const StateConversion*, OperationIdHash> states_;
    std::unordered_set<ValueId, ValueIdHash> inputs_;
    std::unordered_map<ValueId, std::optional<uint64_t>, ValueIdHash> memo_;
};

std::optional<OperationKind> convertedKind(OperationKind kind)
{
    switch (kind)
    {
    case OperationKind::kRegisterWritePort: return kWrite;
    case OperationKind::kRegisterWriteMaskPort: return kWriteMask;
    case OperationKind::kRegisterWriteArrayPort: return kWriteArray;
    case OperationKind::kRegisterWriteArrayMaskPort: return kWriteArrayMask;
    case OperationKind::kRegisterFillPort: return kFill;
    case OperationKind::kLatchWritePort: return kLatchWrite;
    case OperationKind::kSystemTask: return kSystemTask;
    case OperationKind::kSystemFunction: return kSystemFunction;
    case OperationKind::kDpicCall: return kDpiCall;
    default: return std::nullopt;
    }
}

bool scalarBit(const Graph& graph, ValueId value)
{
    const auto type = graph.valueLogicalType(value);
    return type.kind == ValueType::Logic && type.width == 1 && type.shape.empty();
}

bool scalarCondition(const Graph& graph, ValueId value)
{
    const auto type = graph.valueLogicalType(value);
    return type.kind == ValueType::Logic && type.shape.empty();
}

ValueId constant(Graph& graph, const LogicalType& type, std::string literal)
{
    const auto value = graph.createValue(type);
    const auto operation = graph.createOperation(OperationKind::kConstant);
    graph.addResult(operation, value);
    graph.setAttr(operation, "constValue", std::move(literal));
    return value;
}
} // namespace

GrhToGrhsimPass::GrhToGrhsimPass(GrhToGrhsimOptions options)
    : Pass("grh-to-grhsim", "grh-to-grhsim", "Convert simulation ops on the unpartitioned GRH graph"),
      options_(std::move(options))
{}

bool GrhToGrhsimPass::supportsDialect(std::string_view dialect) const noexcept
{
    return dialect == "grh" || dialect == "grhsim" || dialect == "grhsim.cpu";
}

PassResult GrhToGrhsimPass::run()
{
    PassResult result;
    Graph* graph = nullptr;
    try
    {
        const auto domain = options_.logicDomain;
        if (domain != LogicDomain::TwoState && domain != LogicDomain::FourState)
            throw std::invalid_argument("Unknown logic domain");
        std::string top = options_.top;
        if (top.empty())
        {
            if (design().topGraphs().size() != 1)
                throw std::invalid_argument("Select exactly one top graph with -top");
            top = design().topGraphs()[0];
        }
        graph = design().findGraph(top);
        if (!graph) throw std::invalid_argument("Top graph does not exist: " + top);
        if (const auto stage = graph->metadata(kStage))
        {
            if (*stage != AttributeValue(std::string("converted")) ||
                graph->metadata("grhsim.logicDomain") != std::optional<AttributeValue>(std::string(toString(domain))))
                throw std::invalid_argument("Conversion requires a base graph or matching converted checkpoint");
            if (const auto problem = verifyGraph(*graph); !problem.empty()) throw std::invalid_argument(problem);
            for (const auto op : graph->operations())
                if (const auto problem = verifyOpDescriptor(*graph, op); !problem.empty())
                    throw std::invalid_argument(problem);
            return result;
        }
        PassManager verifier;
        verifier.addPass(std::make_unique<GrhVerifyPass>());
        if (!verifier.run(design(), diags()).success) return {false, true, {}};

        // Preflight all source contracts before changing types or operations.
        const std::vector<OperationId> operations(graph->operations().begin(), graph->operations().end());
        std::unordered_map<OperationId, ValueType, OperationIdHash> readKinds;
        for (const auto op : operations)
        {
            const auto kind = graph->opKind(op);
            const auto* descriptor = findOpDescriptor(kind);
            if (!descriptor || descriptor->dialect != "grh" || graph->getOperation(op).attr(kPartition))
                throw std::invalid_argument("Expected an unpartitioned base GRH graph");
            if (kind == OperationKind::kInstance || kind == OperationKind::kBlackbox ||
                kind == OperationKind::kXMRRead || kind == OperationKind::kXMRWrite)
                throw std::invalid_argument("grh-to-grhsim requires a fully expanded top graph");
            if (isRegisterReadPortKind(kind) || kind == OperationKind::kLatchReadPort)
            {
                const auto target = graph->findOperation(attribute<std::string>(*graph, op,
                    kind == OperationKind::kLatchReadPort ? "latchSymbol" : "regSymbol").value());
                const auto valueKind = graph->valueType(graph->opResults(op)[0]);
                const auto [it, inserted] = readKinds.emplace(target, valueKind);
                if (!inserted && it->second != valueKind)
                    throw std::invalid_argument("State readers disagree on the value kind");
            }
        }
        std::vector<StateConversion> states;
        for (const auto op : operations)
        {
            const auto kind = graph->opKind(op);
            if (kind != OperationKind::kRegister && kind != OperationKind::kLatch) continue;
            auto type = stateLogicalType(*graph, op);
            if (!graph->getOperation(op).attr("valueType") && readKinds.contains(op)) type.kind = readKinds.at(op);
            if (type.kind == ValueType::Logic) type.domain = domain;
            type = type.normalized();
            InitSpec init;
            if (type.kind != ValueType::Logic && !graph->getOperation(op).attr("initSpecKinds"))
            {
                const auto value = attribute<std::string>(*graph, op, "initValue").value_or(type.kind == ValueType::Real ? "0" : "");
                init.push_back({value == "$random" ? InitKind::Random : InitKind::Constant,
                               value == "$random" ? std::string{} : value});
            }
            else init = stateInitSpec(*graph, op);
            validateInitSpec(type, init);
            states.push_back({op, type, std::move(init)});
        }
        EventInitialValue initial(*graph, states, domain);
        std::vector<OpConversion> conversions;
        int64_t writerOrder = 0;
        for (const auto op : operations)
        {
            const auto kind = convertedKind(graph->opKind(op));
            if (!kind) continue;
            for (const auto key : {"event_edges", "TriggerSpec", "triggerMode"})
                if (graph->getOperation(op).attr(key))
                    throw std::invalid_argument("Source operation has an unsupported trigger annotation: " + std::string(key));
            if (const auto polarity = attribute<std::string>(*graph, op, "clkPolarity")) {
                const auto edges = attribute<std::vector<std::string>>(*graph, op, "eventEdge").value_or(std::vector<std::string>{});
                if ((*kind != kSystemTask && *kind != kDpiCall) || edges.size() != 1 || edges[0] != *polarity)
                    throw std::invalid_argument("Call clkPolarity must agree with its single eventEdge");
            }
            OpConversion conversion{op, *kind, {}};
            const auto args = graph->opOperands(op);
            if (*kind == kSystemTask || *kind == kDpiCall)
            {
                if (const auto process = attribute<std::string>(*graph, op, "procKind"); process && process->empty())
                    throw std::invalid_argument("Call process kind must not be empty");
                attribute<bool>(*graph, op, "hasTiming");
            }
            if (*kind == kSystemFunction)
            {
                if (graph->opResults(op).size() != 1 || attribute<std::string>(*graph, op, "name").value_or("").empty())
                    throw std::invalid_argument("System function requires a name and one result");
                if (graph->getOperation(op).attr("eventEdge"))
                    throw std::invalid_argument("System function must not carry trailing events");
                systemFunctionEffect(*graph, op);
                for (const auto key : {"sampleId", "sampleScope"})
                    if (const auto text = attribute<std::string>(*graph, op, key); text && text->empty())
                        throw std::invalid_argument("Function sample identity/scope must not be empty");
            }
            else if (*kind != kLatchWrite)
            {
                const auto edges = attribute<std::vector<std::string>>(*graph, op, "eventEdge").value_or(std::vector<std::string>{});
                if (edges.size() >= args.size() || !scalarCondition(*graph, args[0]))
                    throw std::invalid_argument("Malformed event consumer operands or condition: " +
                        std::string(graph->getOperation(op).symbolText()) + " (" + std::string(toString(graph->opKind(op))) +
                        ", operands=" + std::to_string(args.size()) + ", events=" + std::to_string(edges.size()) +
                        (args.empty() ? ")" : ", condition=" + std::string(toString(graph->valueType(args[0]))) +
                        "[" + std::to_string(graph->valueWidth(args[0])) + "])"));
                if (isWriter(*kind) && edges.empty()) throw std::invalid_argument("Clocked writer has no events");
                for (std::size_t i = 0; i < edges.size(); ++i)
                {
                    const auto event = args[args.size() - edges.size() + i];
                    if (!scalarBit(*graph, event) || (edges[i] != "posedge" && edges[i] != "negedge" && edges[i] != "both"))
                        throw std::invalid_argument("Event requires a scalar bit and posedge/negedge/both");
                    const auto bits = initial.eval(event);
                    conversion.events.push_back({event, edges[i], bits && (*bits & 1) ? "1'h1" : "1'h0", !bits});
                }
                if (*kind == kSystemTask && (!graph->opResults(op).empty() ||
                    attribute<std::string>(*graph, op, "name").value_or("").empty()))
                    throw std::invalid_argument("System task requires a name and no results");
                if (*kind == kDpiCall) {
                    if (const auto problem = verifyDpiSignature(*graph, op, 1, edges.size(), true); !problem.empty())
                        throw std::invalid_argument(problem);
                    const auto imported = graph->findOperation(attribute<std::string>(*graph, op, "targetImportSymbol").value());
                    const auto directions = attribute<std::vector<std::string>>(*graph, imported, "argsDirection").value_or(std::vector<std::string>{});
                    const auto signs = attribute<std::vector<bool>>(*graph, imported, "argsSigned").value_or(std::vector<bool>{});
                    std::size_t input = 1;
                    for (const std::string_view direction : {"input", "inout"})
                        for (std::size_t i = 0; i < directions.size(); ++i) {
                            if (directions[i] != direction) continue;
                            auto type = graph->valueLogicalType(args[input]);
                            if (type.kind == ValueType::Logic && type.isSigned != signs[i]) {
                                type.isSigned = signs[i]; type.domain = domain;
                                conversion.inputViews.emplace_back(input, std::move(type));
                            }
                            ++input;
                        }
                }
            }
            else if (args.empty() || !scalarCondition(*graph, args[0]))
                throw std::invalid_argument("Latch writer requires a scalar Logic enable");
            if (isWriter(*kind))
            {
                conversion.writerOrder = attribute<int64_t>(*graph, op, "writerOrder").value_or(writerOrder);
                if (conversion.writerOrder < 0) throw std::invalid_argument("Negative writerOrder");
                ++writerOrder;
            }
            conversions.push_back(std::move(conversion));
        }
        std::vector<std::pair<OperationId, std::string>> constants;
        for (const auto op : operations)
            if (graph->opKind(op) == OperationKind::kConstant && graph->opResults(op).size() == 1)
            {
                const auto value = graph->opResults(op)[0];
                if (graph->valueType(value) == ValueType::Logic)
                    constants.emplace_back(op, projectLiteral(attribute<std::string>(*graph, op, "constValue").value(),
                        graph->valueWidth(value), graph->valueSigned(value), domain));
            }
        std::unordered_set<ValueId, ValueIdHash> inputs;
        for (const auto& port : graph->inoutPorts()) inputs.insert(port.in);
        std::vector<ValueId> undriven;
        for (const auto value : graph->values())
        {
            if (graph->valueDef(value) || graph->valueIsInput(value) || inputs.contains(value)) continue;
            const auto detached = graph->getValue(value);
            if (detached.users().empty() && !detached.isOutput() && !detached.isInout()) continue;
            if (domain != LogicDomain::TwoState || detached.type() != ValueType::Logic)
                throw std::invalid_argument("Referenced undriven values require explicit TwoState projection");
            undriven.push_back(value);
        }
        for (auto& state : states)
            if (state.type.kind == ValueType::Logic)
                for (auto& step : state.init)
                    if (step.kind == InitKind::Constant || (step.kind == InitKind::Fill && !step.random))
                        step.value = projectLiteral(step.value,
                            step.kind == InitKind::Fill ? state.type.leafWidth() : state.type.width,
                            state.type.isSigned, domain);

        result.changed = true;
        const std::vector<ValueId> values(graph->values().begin(), graph->values().end());
        for (const auto value : values)
        {
            auto type = graph->valueLogicalType(value);
            if (type.kind == ValueType::Logic) { type.domain = domain; graph->setValueLogicalType(value, type); }
        }
        for (const auto& [op, literal] : constants) graph->setAttr(op, "constValue", literal);
        for (const auto& state : states)
        {
            setStateLogicalType(*graph, state.declaration, state.type);
            setStateInitSpec(*graph, state.declaration, state.init);
        }
        for (const auto value : undriven)
        {
            const auto op = graph->createOperation(OperationKind::kConstant);
            graph->addResult(op, value);
            graph->setAttr(op, "constValue", std::to_string(graph->valueWidth(value)) + "'h0");
            graph->setOpSrcLoc(op, makeTransformSrcLoc(id(), "undriven-zero"));
        }
        const LogicalType pulseType{ValueType::Logic, 1, false, LogicDomain::TwoState, {}};
        ValueId always;
        for (const auto& conversion : conversions)
        {
            const auto op = conversion.operation;
            if (conversion.kind != kSystemFunction && !scalarBit(*graph, graph->opOperands(op)[0])) {
                const auto condition = graph->createOperation(OperationKind::kReduceOr);
                const auto bit = graph->createValue(pulseType);
                graph->addOperand(condition, graph->opOperands(op)[0]);
                graph->addResult(condition, bit);
                graph->replaceOperand(op, 0, bit);
                graph->setOpSrcLoc(condition, makeTransformSrcLoc(id(), "call-condition"));
                inheritOrigin(*graph, condition, {op});
            }
            for (const auto& [index, type] : conversion.inputViews) {
                const auto view = graph->createOperation(OperationKind::kAssign);
                const auto value = graph->createValue(type);
                graph->addOperand(view, graph->opOperands(op)[index]);
                graph->addResult(view, value);
                graph->replaceOperand(op, index, value);
                graph->setOpSrcLoc(view, makeTransformSrcLoc(id(), "dpi-input-view"));
                inheritOrigin(*graph, view, {op});
            }
            ValueId fire;
            for (const auto& event : conversion.events)
            {
                const auto detector = graph->createOperation(kEdgeDetect);
                const auto pulse = graph->createValue(pulseType);
                graph->addOperand(detector, event.value);
                graph->addResult(detector, pulse);
                graph->setAttr(detector, "edge", event.edge);
                graph->setAttr(detector, "prevInit", event.previous);
                graph->setAttr(detector, "sampleScope", std::string("event-round"));
                graph->setAttr(detector, "historyUpdate", std::string("unconditional"));
                if (event.fallback) graph->setAttr(detector, "prevInitFallback", true);
                graph->setOpSrcLoc(detector, makeTransformSrcLoc(id(), "edge-detect"));
                inheritOrigin(*graph, detector, {op, graph->valueDef(event.value)});
                if (fire)
                {
                    const auto joined = graph->createValue(pulseType);
                    const auto join = graph->createOperation(OperationKind::kOr);
                    graph->addOperand(join, fire);
                    graph->addOperand(join, pulse);
                    graph->addResult(join, joined);
                    inheritOrigin(*graph, join, {op});
                    fire = joined;
                }
                else fire = pulse;
            }
            if (conversion.kind != kLatchWrite && conversion.kind != kSystemFunction)
            {
                for (std::size_t i = 0; i < conversion.events.size(); ++i)
                    graph->eraseOperand(op, graph->opOperands(op).size() - 1);
                if (!fire)
                {
                    if (!always) always = constant(*graph, pulseType, "1'h1");
                    fire = always;
                }
                graph->insertOperand(op, 0, fire);
            }
            graph->setOpKind(op, conversion.kind);
            graph->eraseAttr(op, "eventEdge");
            graph->eraseAttr(op, "clkPolarity");
            if (isWriter(conversion.kind)) graph->setAttr(op, "writerOrder", conversion.writerOrder);
            if (conversion.kind == kSystemTask || conversion.kind == kDpiCall)
            {
                graph->setAttr(op, "eventDriven", !conversion.events.empty());
                if (!graph->getOperation(op).attr("procKind"))
                    graph->setAttr(op, "procKind", std::string(conversion.events.empty() ? "always_comb" : "always"));
                if (!graph->getOperation(op).attr("hasTiming")) graph->setAttr(op, "hasTiming", !conversion.events.empty());
            }
            if (conversion.kind == kSystemFunction)
            {
                const auto effect = systemFunctionEffect(*graph, op);
                graph->setAttr(op, "effect", std::string(effect));
                if (effect != "pure")
                {
                    if (!graph->getOperation(op).attr("sampleId"))
                        graph->setAttr(op, "sampleId", "grhsim.sample/" + std::string(graph->getOperation(op).symbolText()));
                    if (!graph->getOperation(op).attr("sampleScope")) graph->setAttr(op, "sampleScope", std::string("eval"));
                }
            }
            if (const auto problem = verifyOpDescriptor(*graph, op); !problem.empty()) throw std::invalid_argument(problem);
        }
        graph->setMetadata("grhsim.logicDomain", std::string(toString(domain)));
        graph->setMetadata(kStage, std::string("converted"));
        if (!verifier.run(design(), diags()).success) throw std::invalid_argument("Converted graph verification failed");
        logInfo("converted operations=" + std::to_string(conversions.size()) + " states=" + std::to_string(states.size()));
    }
    catch (const std::exception& exception)
    {
        if (graph && result.changed) graph->setMetadata(kStage, std::string("conversion-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{});
        result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform::grhsim
