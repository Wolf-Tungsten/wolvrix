#include "transform/grhsim/grh-to-grhsim.hpp"

#include "transform/grhsim/ops.hpp"
#include "transform/verify.hpp"
#include "ingest/node_attributes.hpp"

#include "slang/numeric/SVInt.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
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
    std::optional<std::string> literalFormat;
    ValueId literalFormatValue;
};

struct EventKey {
    ValueId value;
    std::string_view edge;
    std::string_view previous;
    bool fallback;
    std::string_view sampleScope;
    std::string_view historyUpdate;

    bool operator==(const EventKey&) const = default;
};

struct EventKeyHash {
    std::size_t operator()(const EventKey& key) const noexcept {
        std::size_t hash = ValueIdHash{}(key.value);
        const auto mix = [&](std::size_t value) {
            hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        };
        mix(std::hash<std::string_view>{}(key.edge));
        mix(std::hash<std::string_view>{}(key.previous));
        mix(std::hash<bool>{}(key.fallback));
        mix(std::hash<std::string_view>{}(key.sampleScope));
        mix(std::hash<std::string_view>{}(key.historyUpdate));
        return hash;
    }
};

struct JoinKey {
    ValueId left;
    ValueId right;
    bool operator==(const JoinKey&) const = default;
};

struct JoinKeyHash {
    std::size_t operator()(const JoinKey& key) const noexcept {
        const auto left = ValueIdHash{}(key.left);
        const auto right = ValueIdHash{}(key.right);
        return left ^ (right + 0x9e3779b9u + (left << 6) + (left >> 2));
    }
};

// Keep source references outside the append-only graph pools until each shared
// helper's complete provenance is known. Repeated consumers of a large clock
// neither rescan its identities nor leave every intermediate union in the pool.
class HelperMetadata {
public:
    std::size_t add(OperationId operation, ValueId value) {
        records_.push_back({operation, value});
        return records_.size() - 1;
    }

    void addOperation(std::size_t index, OperationId source, bool origin) {
        if (!source) return;
        auto& record = records_[index];
        const auto [it, inserted] = record.operationIndices.emplace(source, record.operations.size());
        if (inserted) record.operations.push_back({source, origin});
        else record.operations[it->second].origin |= origin;
    }

    void addValue(std::size_t index, ValueId source) {
        if (!source) return;
        auto& record = records_[index];
        if (record.valueSet.insert(source).second) record.values.push_back(source);
    }

    void publish(Graph& graph) const {
        std::vector<SemanticId> ids;
        std::unordered_set<SemanticId> seen;
        std::vector<std::string> origins;
        for (const auto& record : records_) {
            ids.clear(); seen.clear(); origins.clear();
            // Each source role retains first-use order; operation identities
            // precede value identities on the shared operation and its result.
            const auto append = [&](std::span<const SemanticId> source) {
                for (const auto id : source)
                    if (seen.insert(id).second) ids.push_back(id);
            };
            for (const auto& source : record.operations) {
                append(graph.opSemanticIds(source.operation));
                if (source.origin)
                    if (const auto paths = getOriginModules(graph, source.operation))
                        appendOriginModules(origins, *paths);
            }
            for (const auto source : record.values) append(graph.valueSemanticIds(source));
            graph.setOpSemanticIds(record.operation, ids);
            graph.setValueSemanticIds(record.value, ids);
            if (!origins.empty()) graph.setAttr(record.operation, kOriginModulesKey, origins);
        }
    }

private:
    struct OperationSource {
        OperationId operation;
        bool origin;
    };
    struct Record {
        OperationId operation;
        ValueId value;
        std::vector<OperationSource> operations;
        std::unordered_map<OperationId, std::size_t, OperationIdHash> operationIndices;
        std::vector<ValueId> values;
        std::unordered_set<ValueId, ValueIdHash> valueSet;
    };
    std::vector<Record> records_;
};

struct EventHelper {
    ValueId pulse;
    std::size_t metadata;
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
    const auto value = graph.createTemporaryValue(type);
    const auto operation = graph.createTemporaryOperation(OperationKind::kConstant);
    graph.addResult(operation, value);
    graph.setAttr(operation, "constValue", std::move(literal));
    return value;
}

// Conversion synthesizes a small number of helper operations (edge detectors,
// reductions, views and joins).  Keep their source identity in the compact
// SemanticTree pools instead of manufacturing source-name strings for every
// helper.  Operation identities and result-value identities are transferred
// together because later GRH passes may retain either row independently.
void inheritSemantic(Graph& graph, OperationId targetOperation, ValueId targetValue,
                     std::initializer_list<OperationId> sourceOperations,
                     std::initializer_list<ValueId> sourceValues)
{
    for (const auto source : sourceOperations)
    {
        if (!source.valid()) continue;
        graph.mergeOpSemanticIds(targetOperation, source);
        if (targetValue.valid()) graph.addValueSemanticIds(targetValue, graph.opSemanticIds(source));
    }
    for (const auto source : sourceValues)
    {
        if (!source.valid()) continue;
        const auto semantics = graph.valueSemanticIds(source);
        if (semantics.empty()) continue;
        graph.addOpSemanticIds(targetOperation, semantics);
        // The operation update can reallocate the shared semantic pool.
        if (targetValue.valid()) graph.addValueSemanticIds(targetValue, graph.valueSemanticIds(source));
    }
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
    const bool phaseTiming = std::getenv("WOLVRIX_SIMPLIFY_PHASE_TIMING") != nullptr;
    const auto timingStart = std::chrono::steady_clock::now();
    auto previousPhase = timingStart;
    const auto phase = [&](const char* name) {
        if (!phaseTiming) return;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - previousPhase).count();
        const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(now - timingStart).count();
        std::fprintf(stderr, "[grh-to-grhsim-phase] phase=%s elapsed_ms=%lld total_ms=%lld operations=%zu values=%zu\n",
                     name, static_cast<long long>(elapsed), static_cast<long long>(total),
                     graph ? graph->operations().size() : 0, graph ? graph->values().size() : 0);
        previousPhase = now;
    };
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
        // Pre-F08 checkpoints can still be inspected, but cannot be upgraded
        // into a node-aware simulation merely by attaching a new policy.
        if (graph->metadata(ingest::node::kScopePolicy)) ingest::node::requirePolicy(*graph);
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
        phase("preverify-start");
        PassManager verifier;
        verifier.addPass(std::make_unique<GrhVerifyPass>());
        if (!verifier.run(design(), diags()).success) {
            phase("preverify-failed");
            return {false, true, {}};
        }
        phase("preverify-done");

        // Preflight all source contracts before changing types or operations.
        const std::vector<OperationId> operations(graph->operations().begin(), graph->operations().end());
        std::unordered_map<OperationId, ValueType, OperationIdHash> readKinds;
        for (const auto op : operations)
        {
            const auto kind = graph->opKind(op);
            const auto* descriptor = findOpDescriptor(kind);
            if (!descriptor || descriptor->dialect != "grh" || graph->opAttrRange(op).find(kPartition))
                throw std::invalid_argument("Expected an unpartitioned base GRH graph");
            if (graph->opAttrRange(op).find(kLiteralFormat))
                throw std::invalid_argument("Base GRH must not carry grhsim.literalFormat");
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
        phase("operation-contracts-done");
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
        phase("state-contracts-done");
        EventInitialValue initial(*graph, states, domain);
        std::vector<OpConversion> conversions;
        std::unordered_map<std::string, std::unordered_set<int64_t>> usedWriterOrders;
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
                if (*kind == kSystemTask && args.size() > edges.size() + 1 &&
                    attribute<std::string>(*graph, op, "name") == "write" &&
                    attribute<std::string>(*graph, op, "firrtl.effectKind") == "printf")
                {
                    const auto format = args[1];
                    const auto type = graph->valueLogicalType(format);
                    const auto producer = graph->valueDef(format);
                    if (type.kind == ValueType::String && type.shape.empty() && producer &&
                        graph->opKind(producer) == OperationKind::kConstant &&
                        graph->opOperands(producer).empty() && graph->opResults(producer).size() == 1)
                    {
                        conversion.literalFormat = attribute<std::string>(*graph, producer, "constValue").value();
                        conversion.literalFormatValue = format;
                    }
                }
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
                const auto target = attribute<std::string>(*graph, op,
                    *kind == kLatchWrite ? "latchSymbol" : "regSymbol").value();
                auto& used = usedWriterOrders[target];
                if (const auto explicitOrder = attribute<int64_t>(*graph, op, "writerOrder"))
                {
                    if (*explicitOrder < 0) throw std::invalid_argument("Negative writerOrder");
                    if (!used.insert(*explicitOrder).second)
                        throw std::invalid_argument("Duplicate writerOrder for state '" + target + "'");
                    conversion.writerOrder = *explicitOrder;
                }
                else
                {
                    while (used.contains(writerOrder)) ++writerOrder;
                    conversion.writerOrder = writerOrder;
                    used.insert(writerOrder);
                    ++writerOrder;
                }
            }
            conversions.push_back(std::move(conversion));
        }
        phase("event-contracts-done");
        std::vector<std::pair<OperationId, std::string>> constants;
        for (const auto op : operations)
            if (graph->opKind(op) == OperationKind::kConstant && graph->opResults(op).size() == 1)
            {
                const auto value = graph->opResults(op)[0];
                if (graph->valueType(value) == ValueType::Logic)
                    constants.emplace_back(op, projectLiteral(attribute<std::string>(*graph, op, "constValue").value(),
                        graph->valueWidth(value), graph->valueSigned(value), domain));
            }
        phase("constant-literals-prepared");
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
        phase("undriven-scan-done");
        for (auto& state : states)
            if (state.type.kind == ValueType::Logic)
                for (auto& step : state.init)
                    if (step.kind == InitKind::Constant || (step.kind == InitKind::Fill && !step.random))
                        step.value = projectLiteral(step.value,
                            step.kind == InitKind::Fill ? state.type.leafWidth() : state.type.width,
                            state.type.isSigned, domain);

        phase("state-literals-prepared");
        result.changed = true;
        const std::vector<ValueId> values(graph->values().begin(), graph->values().end());
        for (const auto value : values)
        {
            auto type = graph->valueLogicalType(value);
            if (type.kind == ValueType::Logic) { type.domain = domain; graph->setValueLogicalType(value, type); }
        }
        phase("value-domains-updated");
        for (const auto& [op, literal] : constants) graph->setAttr(op, "constValue", literal);
        phase("constant-literals-updated");
        for (const auto& state : states)
        {
            setStateLogicalType(*graph, state.declaration, state.type);
            setStateInitSpec(*graph, state.declaration, state.init);
        }
        phase("states-updated");
        for (const auto value : undriven)
        {
            const auto op = graph->createTemporaryOperation(OperationKind::kConstant);
            graph->addResult(op, value);
            graph->setAttr(op, "constValue", std::to_string(graph->valueWidth(value)) + "'h0");
            graph->setOpSrcLoc(op, makeTransformSrcLoc(id(), "undriven-zero"));
            inheritSemantic(*graph, op, value, {}, {value});
        }
        phase("undriven-materialized");
        const LogicalType pulseType{ValueType::Logic, 1, false, LogicDomain::TwoState, {}};
        ValueId always;
        HelperMetadata helperMetadata;
        // Key string_views refer to the completed, immutable conversions list.
        std::unordered_map<EventKey, EventHelper, EventKeyHash> eventHelpers;
        std::unordered_map<JoinKey, EventHelper, JoinKeyHash> joinHelpers;
        struct RewriteStats {
            uint64_t originNs = 0, semanticNs = 0, descriptorNs = 0;
            uint64_t sourceOpIds = 0, sourceValueIds = 0, sourceOriginPaths = 0;
            std::size_t maxOpIds = 0, maxValueIds = 0, maxOriginPaths = 0;
            uint32_t maxOp = 0, maxValue = 0, maxOriginOp = 0;
            std::size_t events = 0, reductions = 0, views = 0, detectors = 0, joins = 0;
        } rewriteStats;
        const auto rewriteStart = std::chrono::steady_clock::now();
        const auto nsSince = [](auto start) {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
        };
        const auto opSource = [&](OperationId source) {
            if (!phaseTiming || !source) return std::size_t{0};
            const auto size = graph->opSemanticIds(source).size();
            rewriteStats.sourceOpIds += size;
            if (size > rewriteStats.maxOpIds) { rewriteStats.maxOpIds = size; rewriteStats.maxOp = source.index; }
            if (const auto attr = graph->opAttrRange(source).find(kOriginModulesKey))
                if (const auto origins = attr->value().get<std::vector<std::string>>()) {
                    rewriteStats.sourceOriginPaths += origins->size();
                    if (origins->size() > rewriteStats.maxOriginPaths) {
                        rewriteStats.maxOriginPaths = origins->size(); rewriteStats.maxOriginOp = source.index;
                    }
                }
            return size;
        };
        const auto valueSource = [&](ValueId source) {
            if (!phaseTiming || !source) return std::size_t{0};
            const auto size = graph->valueSemanticIds(source).size();
            rewriteStats.sourceValueIds += size;
            if (size > rewriteStats.maxValueIds) { rewriteStats.maxValueIds = size; rewriteStats.maxValue = source.index; }
            return size;
        };
        if (phaseTiming) {
            for (const auto& conversion : conversions) {
                opSource(conversion.operation);
                for (const auto& event : conversion.events) {
                    ++rewriteStats.events; opSource(graph->valueDef(event.value)); valueSource(event.value);
                }
                if (conversion.kind != kSystemFunction) {
                    const auto condition = graph->opOperands(conversion.operation)[0];
                    opSource(graph->valueDef(condition)); valueSource(condition);
                }
                for (const auto& [index, type] : conversion.inputViews) {
                    const auto input = graph->opOperands(conversion.operation)[index];
                    opSource(graph->valueDef(input)); valueSource(input);
                }
            }
            std::fprintf(stderr, "[grh-to-grhsim-rewrite] census conversions=%zu events=%zu op_ids_sum=%llu max_op_ids=%zu op=%u value_ids_sum=%llu max_value_ids=%zu value=%u origin_paths_sum=%llu max_origin_paths=%zu origin_op=%u\n",
                conversions.size(), rewriteStats.events, static_cast<unsigned long long>(rewriteStats.sourceOpIds),
                rewriteStats.maxOpIds, rewriteStats.maxOp, static_cast<unsigned long long>(rewriteStats.sourceValueIds),
                rewriteStats.maxValueIds, rewriteStats.maxValue, static_cast<unsigned long long>(rewriteStats.sourceOriginPaths),
                rewriteStats.maxOriginPaths, rewriteStats.maxOriginOp);
            // The census totals count pre-rewrite sources once per role. The
            // following totals count sources at each actual propagation call.
            rewriteStats.sourceOpIds = rewriteStats.sourceValueIds = rewriteStats.sourceOriginPaths = 0;
        }
        std::size_t rewriteOrdinal = 0, tracedLargeRecords = 0;
        bool traceRecord = false;
        const auto mark = [&](const char* bucket, const char* state, OperationId target) {
            if (phaseTiming && traceRecord)
                std::fprintf(stderr, "[grh-to-grhsim-rewrite] ordinal=%zu bucket=%s state=%s target=%u elapsed_ms=%llu\n",
                    rewriteOrdinal, bucket, state, target.index, static_cast<unsigned long long>(nsSince(rewriteStart) / 1000000));
        };
        const auto inheritOriginTimed = [&](OperationId target, std::initializer_list<OperationId> sources) {
            for (const auto source : sources) opSource(source);
            mark("origin", "start", target);
            const auto start = phaseTiming ? std::chrono::steady_clock::now() : rewriteStart;
            inheritOrigin(*graph, target, sources);
            if (phaseTiming) rewriteStats.originNs += nsSince(start);
            mark("origin", "done", target);
        };
        const auto inheritSemanticTimed = [&](OperationId target, ValueId value,
                std::initializer_list<OperationId> ops, std::initializer_list<ValueId> values) {
            for (const auto source : ops) opSource(source);
            for (const auto source : values) valueSource(source);
            mark("semantic", "start", target);
            const auto start = phaseTiming ? std::chrono::steady_clock::now() : rewriteStart;
            inheritSemantic(*graph, target, value, ops, values);
            if (phaseTiming) rewriteStats.semanticNs += nsSince(start);
            mark("semantic", "done", target);
        };
        for (const auto& conversion : conversions)
        {
            const auto op = conversion.operation;
            ++rewriteOrdinal;
            traceRecord = rewriteOrdinal <= 8;
            if (phaseTiming) {
                bool largeSource = graph->opSemanticIds(op).size() > 1024;
                for (const auto& event : conversion.events) {
                    const auto source = graph->valueDef(event.value);
                    largeSource |= graph->valueSemanticIds(event.value).size() > 1024 ||
                        (source && graph->opSemanticIds(source).size() > 1024);
                }
                if (conversion.kind != kSystemFunction) {
                    const auto value = graph->opOperands(op)[0];
                    const auto source = graph->valueDef(value);
                    largeSource |= graph->valueSemanticIds(value).size() > 1024 ||
                        (source && graph->opSemanticIds(source).size() > 1024);
                }
                for (const auto& [index, type] : conversion.inputViews) {
                    const auto value = graph->opOperands(op)[index];
                    const auto source = graph->valueDef(value);
                    largeSource |= graph->valueSemanticIds(value).size() > 1024 ||
                        (source && graph->opSemanticIds(source).size() > 1024);
                }
                // Bound verbose tracing when thousands of writers share one
                // large clock; retain periodic cumulative progress thereafter.
                if (largeSource) traceRecord |= tracedLargeRecords++ < 32;
                if (traceRecord)
                    std::fprintf(stderr, "[grh-to-grhsim-rewrite] ordinal=%zu/%zu op=%u events=%zu state=start elapsed_ms=%llu\n",
                        rewriteOrdinal, conversions.size(), op.index, conversion.events.size(),
                        static_cast<unsigned long long>(nsSince(rewriteStart) / 1000000));
            }
            if (conversion.kind != kSystemFunction && !scalarBit(*graph, graph->opOperands(op)[0])) {
                const auto sourceCondition = graph->opOperands(op)[0];
                const auto sourceConditionDef = graph->valueDef(sourceCondition);
                const auto condition = graph->createTemporaryOperation(OperationKind::kReduceOr);
                ++rewriteStats.reductions;
                const auto bit = graph->createTemporaryValue(pulseType);
                graph->addOperand(condition, sourceCondition);
                graph->addResult(condition, bit);
                graph->replaceOperand(op, 0, bit);
                graph->setOpSrcLoc(condition, makeTransformSrcLoc(id(), "call-condition"));
                inheritOriginTimed(condition, {op});
                inheritSemanticTimed(condition, bit, {op, sourceConditionDef}, {sourceCondition});
            }
            for (const auto& [index, type] : conversion.inputViews) {
                const auto sourceValue = graph->opOperands(op)[index];
                const auto sourceDef = graph->valueDef(sourceValue);
                const auto view = graph->createTemporaryOperation(OperationKind::kAssign);
                ++rewriteStats.views;
                const auto value = graph->createTemporaryValue(type);
                graph->addOperand(view, sourceValue);
                graph->addResult(view, value);
                graph->replaceOperand(op, index, value);
                graph->setOpSrcLoc(view, makeTransformSrcLoc(id(), "dpi-input-view"));
                inheritOriginTimed(view, {op});
                inheritSemanticTimed(view, value, {op, sourceDef}, {sourceValue});
            }
            ValueId fire;
            for (std::size_t eventIndex = 0; eventIndex < conversion.events.size(); ++eventIndex)
            {
                const auto& event = conversion.events[eventIndex];
                const auto sourceDef = graph->valueDef(event.value);
                const EventKey key{event.value, event.edge, event.previous, event.fallback,
                                   "event-round", "unconditional"};
                const auto [eventIt, created] = eventHelpers.try_emplace(key);
                if (created) {
                    const auto detector = graph->createTemporaryOperation(kEdgeDetect);
                    const auto pulse = graph->createTemporaryValue(pulseType);
                    ++rewriteStats.detectors;
                    graph->addOperand(detector, event.value);
                    graph->addResult(detector, pulse);
                    graph->setAttr(detector, "edge", event.edge);
                    graph->setAttr(detector, "prevInit", event.previous);
                    graph->setAttr(detector, "sampleScope", std::string(key.sampleScope));
                    graph->setAttr(detector, "historyUpdate", std::string(key.historyUpdate));
                    if (event.fallback) graph->setAttr(detector, "prevInitFallback", true);
                    graph->setOpSrcLoc(detector, makeTransformSrcLoc(id(), "edge-detect"));
                    eventIt->second = {pulse, helperMetadata.add(detector, pulse)};
                }
                const auto& eventHelper = eventIt->second;
                helperMetadata.addOperation(eventHelper.metadata, op, true);
                helperMetadata.addOperation(eventHelper.metadata, sourceDef, true);
                helperMetadata.addValue(eventHelper.metadata, event.value);
                const auto pulse = eventHelper.pulse;
                if (fire)
                {
                    const auto [joinIt, joined] = joinHelpers.try_emplace(JoinKey{fire, pulse});
                    if (joined) {
                        const auto value = graph->createTemporaryValue(pulseType);
                        const auto join = graph->createTemporaryOperation(OperationKind::kOr);
                        ++rewriteStats.joins;
                        graph->addOperand(join, fire);
                        graph->addOperand(join, pulse);
                        graph->addResult(join, value);
                        joinIt->second = {value, helperMetadata.add(join, value)};
                    }
                    const auto& joinHelper = joinIt->second;
                    helperMetadata.addOperation(joinHelper.metadata, op, true);
                    // Shared pulse metadata includes other consumers. Derive
                    // this prefix's identity from its original sources only.
                    for (std::size_t prefix = 0; prefix <= eventIndex; ++prefix) {
                        const auto value = conversion.events[prefix].value;
                        helperMetadata.addOperation(joinHelper.metadata, graph->valueDef(value), false);
                        helperMetadata.addValue(joinHelper.metadata, value);
                    }
                    fire = joinHelper.pulse;
                }
                else fire = pulse;
            }
            if (conversion.kind != kLatchWrite && conversion.kind != kSystemFunction)
            {
                for (std::size_t i = 0; i < conversion.events.size(); ++i)
                    graph->eraseOperand(op, graph->opOperands(op).size() - 1);
                if (conversion.literalFormat)
                {
                    const auto format = conversion.literalFormatValue;
                    inheritSemantic(*graph, op, {}, {graph->valueDef(format)}, {format});
                    graph->setAttr(op, kLiteralFormat, *conversion.literalFormat);
                    graph->eraseOperand(op, 1);
                }
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
                        graph->setAttr(op, "sampleId", "grhsim.sample/" + graph->operationDisplayName(op));
                    if (!graph->getOperation(op).attr("sampleScope")) graph->setAttr(op, "sampleScope", std::string("eval"));
                }
            }
            const auto descriptorStart = phaseTiming ? std::chrono::steady_clock::now() : rewriteStart;
            if (const auto problem = verifyOpDescriptor(*graph, op); !problem.empty()) throw std::invalid_argument(problem);
            if (phaseTiming) rewriteStats.descriptorNs += nsSince(descriptorStart);
            if (phaseTiming && (traceRecord || rewriteOrdinal % 4096 == 0 || rewriteOrdinal == conversions.size())) {
                const auto totalNs = nsSince(rewriteStart);
                std::fprintf(stderr, "[grh-to-grhsim-rewrite] ordinal=%zu/%zu op=%u state=done total_ms=%llu origin_ms=%llu semantic_ms=%llu descriptor_ms=%llu other_ms=%llu reductions=%zu views=%zu detectors=%zu joins=%zu op_ids_sum=%llu value_ids_sum=%llu origin_paths_sum=%llu max_op_ids=%zu max_op=%u max_value_ids=%zu max_value=%u max_origin_paths=%zu max_origin_op=%u\n",
                    rewriteOrdinal, conversions.size(), op.index, static_cast<unsigned long long>(totalNs / 1000000),
                    static_cast<unsigned long long>(rewriteStats.originNs / 1000000),
                    static_cast<unsigned long long>(rewriteStats.semanticNs / 1000000),
                    static_cast<unsigned long long>(rewriteStats.descriptorNs / 1000000),
                    static_cast<unsigned long long>((totalNs - rewriteStats.originNs - rewriteStats.semanticNs - rewriteStats.descriptorNs) / 1000000),
                    rewriteStats.reductions, rewriteStats.views, rewriteStats.detectors, rewriteStats.joins,
                    static_cast<unsigned long long>(rewriteStats.sourceOpIds), static_cast<unsigned long long>(rewriteStats.sourceValueIds),
                    static_cast<unsigned long long>(rewriteStats.sourceOriginPaths), rewriteStats.maxOpIds, rewriteStats.maxOp,
                    rewriteStats.maxValueIds, rewriteStats.maxValue, rewriteStats.maxOriginPaths, rewriteStats.maxOriginOp);
            }
        }
        phase("operation-rewrites-done");
        helperMetadata.publish(*graph);
        phase("helper-metadata-published");
        // Only retire constants whose format edge was removed here. Preserve
        // shared values, public ports and declaration/generate anchors.
        std::unordered_set<uint32_t> anchoredSymbols;
        for (const auto symbol : graph->declaredSymbols()) anchoredSymbols.insert(symbol.value);
        for (const auto& group : graph->generateGroups())
            for (const auto symbol : group.symbols) anchoredSymbols.insert(symbol.value);
        std::unordered_set<ValueId, ValueIdHash> visitedFormats;
        for (const auto& conversion : conversions)
        {
            const auto value = conversion.literalFormatValue;
            if (!conversion.literalFormat || !visitedFormats.insert(value).second ||
                !graph->valueUsers(value).empty() || graph->valueIsInput(value) ||
                graph->valueIsOutput(value) || graph->valueIsInout(value)) continue;
            const auto producer = graph->valueDef(value);
            if (anchoredSymbols.contains(graph->valueSymbol(value).value) ||
                anchoredSymbols.contains(graph->operationSymbol(producer).value)) continue;
            if (!graph->eraseOp(producer) || !graph->eraseValue(value))
                throw std::invalid_argument("Could not retire unused literal task format");
        }
        graph->setMetadata("grhsim.logicDomain", std::string(toString(domain)));
        graph->setMetadata(kStage, std::string("converted"));
        phase("postverify-start");
        if (!verifier.run(design(), diags()).success) throw std::invalid_argument("Converted graph verification failed");
        phase("postverify-done");
        // GrhSIM's legacy ABI still requires symbols, but keep temporary rows
        // symbol-free throughout conversion and all semantic propagation.  Do
        // this single bulk materialization only at the conversion boundary.
        phase("symbol-materialization-start");
        graph->materializeInternalSymbols();
        phase("symbol-materialization-done");
        logInfo("converted operations=" + std::to_string(conversions.size()) + " states=" + std::to_string(states.size()));
    }
    catch (const std::exception& exception)
    {
        phase("failed");
        if (graph && result.changed) graph->setMetadata(kStage, std::string("conversion-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{});
        result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform::grhsim
