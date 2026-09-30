#include "grhsim/pass/lower_edge_detect.hpp"
#include "grhsim/pass/cone_extract.hpp"
#include "grhsim/ir/model.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        const Parameter *findParameter(const GrhSimModel &model,
                                       std::span<const Parameter> parameters,
                                       std::string_view name)
        {
            for (const auto &parameter : parameters)
                if (model.text(parameter.name) == name) return &parameter;
            return nullptr;
        }

        bool isEventConsumerType(std::string_view name) noexcept
        {
            return name == "core.state.regWrite" || name == "core.state.memWrite" ||
                   name == "core.state.memFill" || name == "core.state.memAssign" ||
                   name == "core.state.memWriteSeq" || name == "core.system.task" ||
                   name == "core.dpi.call";
        }

        bool isMemWriteType(std::string_view name) noexcept
        {
            return name == "core.state.memWrite" || name == "core.state.memFill" ||
                   name == "core.state.memAssign" || name == "core.state.memWriteSeq";
        }

        bool validEdge(std::string_view edge) noexcept
        {
            return edge == "posedge" || edge == "negedge" || edge == "both";
        }

        uint64_t widthMask(uint32_t width) noexcept
        {
            return width >= 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
        }

        // Literal rendering matches the init.const constValue style ("1'h0",
        // "8'h00"): explicit width, lowercase hex, zero-padded to full nibbles.
        std::string hexLiteral(uint32_t width, uint64_t bits)
        {
            static const char digits[] = "0123456789abcdef";
            const uint32_t count = (width + 3) / 4;
            std::string text = std::to_string(width) + "'h";
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t shift = (count - 1 - i) * 4;
                const uint8_t nibble =
                    shift < 64 ? static_cast<uint8_t>((bits >> shift) & 0xf) : 0;
                text.push_back(digits[nibble]);
            }
            return text;
        }

        std::optional<uint32_t> explicitLiteralWidth(std::string_view text)
        {
            std::size_t i = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
            if (i == 0 || i >= text.size() || text[i] != '\'') return std::nullopt;
            uint32_t width = 0;
            for (std::size_t j = 0; j < i; ++j)
            {
                width = width * 10 + static_cast<uint32_t>(text[j] - '0');
                if (width > (1u << 20)) return std::nullopt;
            }
            return width;
        }

        std::optional<uint64_t> parseLiteral(std::string_view text, uint32_t width)
        {
            try
            {
                auto bits = slang::SVInt::fromString(std::string(text)).resize(width);
                bits.flattenUnknowns();
                if (!bits.getRawPtr()) return std::nullopt;
                return bits.getRawPtr()[0] & widthMask(width);
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        // Static init-value evaluation over the original event cone. Returns
        // nullopt when the cone is not statically evaluable; the caller falls
        // back to a same-width zero literal with a prev_init_fallback note.
        class ConeInitEval
        {
        public:
            ConeInitEval(const GrhSimModel &model, const std::vector<OpId> &producers)
                : model_(&model), producers_(&producers) {}

            std::optional<uint64_t> eval(ValueId value, uint32_t depth = 0) const
            {
                if (depth > 64 || value.generation != 0 || !value.valid() ||
                    value.index >= producers_->size())
                    return std::nullopt;
                const auto &type = typeOf(value);
                if (type.kind != TypeKind::Logic || type.width == 0 || type.width > 64)
                    return std::nullopt;
                const auto producer = (*producers_)[value.index];
                if (!producer) return std::nullopt;
                const auto &op = model_->operations()[producer.index - 1];
                const auto name = model_->text(op.opType);
                const auto operands = model_->operands(op);
                const auto width = type.width;
                if (name == "core.input.read") return uint64_t{0};
                if (name == "core.compute.constant")
                {
                    const Parameter *literal = findParameter(*model_, model_->parameters(op),
                                                             "constValue");
                    if (!literal) return std::nullopt;
                    if (const auto *text = std::get_if<std::string>(&literal->value))
                        return parseLiteral(*text, width);
                    if (const auto *integer = std::get_if<int64_t>(&literal->value))
                        return parseLiteral(std::to_string(*integer), width);
                    if (const auto *boolean = std::get_if<bool>(&literal->value))
                        return (*boolean ? uint64_t{1} : uint64_t{0}) & widthMask(width);
                    return std::nullopt;
                }
                if (name == "core.state.read")
                {
                    const auto refs = model_->objectRefs(op);
                    if (refs.size() != 1 || refs.front().kind != ObjectKind::State)
                        return std::nullopt;
                    for (const auto &record : model_->initRecords())
                    {
                        if (record.state.index != refs.front().index) continue;
                        for (const auto &step : model_->steps(record))
                        {
                            if (model_->text(step.kind) != "core.init.const") continue;
                            const Parameter *literal = findParameter(
                                *model_, model_->parameters(step), "value");
                            if (!literal) return std::nullopt;
                            const auto *text = std::get_if<std::string>(&literal->value);
                            if (!text) return std::nullopt;
                            // The init literal must be written for exactly this
                            // state's width; an explicit mismatch falls back.
                            if (const auto declared = explicitLiteralWidth(*text))
                                if (*declared != width) return std::nullopt;
                            return parseLiteral(*text, width);
                        }
                        return std::nullopt;
                    }
                    return std::nullopt;
                }
                std::optional<uint64_t> lhs, rhs, third;
                const auto binary = [&](std::size_t count) {
                    if (operands.size() != count) return false;
                    lhs = eval(operands[0], depth + 1);
                    if (!lhs) return false;
                    if (count >= 2)
                    {
                        rhs = eval(operands[1], depth + 1);
                        if (!rhs) return false;
                    }
                    if (count >= 3)
                    {
                        third = eval(operands[2], depth + 1);
                        if (!third) return false;
                    }
                    return true;
                };
                if (name == "core.compute.not")
                {
                    if (!binary(1)) return std::nullopt;
                    return ~*lhs & widthMask(width);
                }
                if (name == "core.compute.and" || name == "core.compute.or" ||
                    name == "core.compute.xor" || name == "core.compute.xnor")
                {
                    if (!binary(2)) return std::nullopt;
                    uint64_t value = 0;
                    if (name == "core.compute.and") value = *lhs & *rhs;
                    else if (name == "core.compute.or") value = *lhs | *rhs;
                    else if (name == "core.compute.xor") value = *lhs ^ *rhs;
                    else value = ~(*lhs ^ *rhs);
                    return value & widthMask(width);
                }
                if (name == "core.compute.logicNot")
                {
                    if (!binary(1)) return std::nullopt;
                    return *lhs == 0 ? uint64_t{1} : uint64_t{0};
                }
                if (name == "core.compute.logicAnd" || name == "core.compute.logicOr")
                {
                    if (!binary(2)) return std::nullopt;
                    if (name == "core.compute.logicAnd")
                        return (*lhs != 0 && *rhs != 0) ? uint64_t{1} : uint64_t{0};
                    return (*lhs != 0 || *rhs != 0) ? uint64_t{1} : uint64_t{0};
                }
                if (name == "core.compute.mux")
                {
                    if (!binary(3)) return std::nullopt;
                    return (*lhs != 0 ? *rhs : *third) & widthMask(width);
                }
                if (name == "core.compute.bitSelect")
                {
                    // operands: mask, whenSet, whenClear — per-bit select.
                    if (!binary(3)) return std::nullopt;
                    return ((*lhs & *rhs) | (~*lhs & *third)) & widthMask(width);
                }
                if (name == "core.compute.sliceStatic")
                {
                    if (!binary(1)) return std::nullopt;
                    const auto parameters = model_->parameters(op);
                    const Parameter *start = findParameter(*model_, parameters, "sliceStart");
                    const Parameter *end = findParameter(*model_, parameters, "sliceEnd");
                    const auto *startValue = start ? std::get_if<int64_t>(&start->value) : nullptr;
                    const auto *endValue = end ? std::get_if<int64_t>(&end->value) : nullptr;
                    if (!startValue || !endValue || *startValue < 0 || *endValue < *startValue ||
                        *endValue >= 64)
                        return std::nullopt;
                    const auto sliceWidth = static_cast<uint32_t>(*endValue - *startValue + 1);
                    return (*lhs >> *startValue) & widthMask(sliceWidth);
                }
                if (name == "core.compute.concat")
                {
                    uint64_t value = 0;
                    uint32_t shift = 0;
                    for (std::size_t i = operands.size(); i-- > 0;)
                    {
                        const auto part = eval(operands[i], depth + 1);
                        if (!part) return std::nullopt;
                        const auto partWidth = typeOf(operands[i]).width;
                        if (shift + partWidth > 64) return std::nullopt;
                        value |= (*part & widthMask(partWidth)) << shift;
                        shift += partWidth;
                    }
                    return value & widthMask(width);
                }
                return std::nullopt;
            }

        private:
            const Type &typeOf(ValueId value) const
            {
                return model_->types()[model_->values()[value.index - 1].type.index - 1];
            }

            const GrhSimModel *model_;
            const std::vector<OpId> *producers_;
        };

        // grhsim.lower-edge-detect: lowers the raw event annotation form
        // (event_edges parameter + trailing event operands, as produced by
        // GRH lowering) into the six-phase P_event form — one deduplicated
        // core.event.edgeDet per (event, edge) cluster plus an Event-phase
        // clone of the event cone — and rewires every consumer to the cluster
        // indices (event_acts parameter). Object refs carry no event slots.
        class LowerEdgeDetectPass final : public Pass
        {
        public:
            LowerEdgeDetectPass()
                : Pass("grhsim.lower-edge-detect", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                struct Consumer
                {
                    OpId id;
                    std::vector<std::string> edges;
                };
                std::vector<Consumer> consumers;
                bool malformed = false;
                for (const auto &op : model.operations())
                {
                    const Parameter *edges =
                        findParameter(model, model.parameters(op), "event_edges");
                    if (!edges) continue;
                    const auto context = "operations[" + std::to_string(op.id.index - 1) + "]";
                    const auto *list = std::get_if<std::vector<std::string>>(&edges->value);
                    const auto opType = model.text(op.opType);
                    if (!list || !isEventConsumerType(opType))
                    {
                        diagnostics.error("event_edges must be a string array on an "
                                          "event-sensitive op",
                                          context);
                        malformed = true;
                        continue;
                    }
                    if (list->size() > model.operands(op).size())
                    {
                        diagnostics.error("event_edges count exceeds the op's operand count",
                                          context);
                        malformed = true;
                        continue;
                    }
                    for (const auto &edge : *list)
                    {
                        if (!validEdge(edge))
                        {
                            diagnostics.error("event_edges entry is not posedge/negedge/both: " +
                                                  edge,
                                              context);
                            malformed = true;
                        }
                    }
                    consumers.push_back(Consumer{op.id, *list});
                }
                if (malformed) return {false, false, {}};
                if (consumers.empty())
                {
                    diagnostics.info("clusters=0 edge_dets=0 rewritten_ops=0 "
                                     "removed_history_states=0 removed_cone_ops=0 "
                                     "prev_init_fallbacks=0",
                                     name());
                    return {true, false, {}};
                }

                // (event value, edge) clusters in first-appearance order: ops are
                // scanned in ascending id, event slots in operand order. act and
                // prev share the cluster index.
                struct Cluster
                {
                    ValueId event;
                    std::string edge;
                    int64_t index = 0;
                    ValueId clone;
                };
                std::vector<Cluster> clusters;
                std::map<std::pair<uint32_t, std::string>, int64_t> clusterByKey;
                for (const auto &consumer : consumers)
                {
                    const auto &op = model.operations()[consumer.id.index - 1];
                    const auto operands = model.operands(op);
                    const std::size_t count = consumer.edges.size();
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto event = operands[operands.size() - count + i];
                        const auto key = std::pair{event.index, consumer.edges[i]};
                        const auto [it, inserted] = clusterByKey.emplace(
                            key, static_cast<int64_t>(clusters.size()));
                        if (!inserted) continue;
                        const auto &type =
                            model.types()[model.values()[event.index - 1].type.index - 1];
                        if (type.kind != TypeKind::Logic)
                        {
                            diagnostics.error("event operand must be a core.logic value",
                                              "operations[" +
                                                  std::to_string(op.id.index - 1) + "]");
                            return {false, false, {}};
                        }
                        clusters.push_back(Cluster{event, consumer.edges[i], it->second, {}});
                    }
                }

                // Clone the event cone into P_event.
                std::vector<ValueId> sinks;
                std::vector<uint8_t> seen(model.values().size() + 1, 0);
                for (const auto &cluster : clusters)
                {
                    if (seen[cluster.event.index]) continue;
                    seen[cluster.event.index] = 1;
                    sinks.push_back(cluster.event);
                }
                auto extraction = extractCone(model, sinks, SimPhase::Event);
                for (auto &cluster : clusters)
                {
                    cluster.clone = extraction.oldToNewValues[cluster.event.index];
                    if (!cluster.clone.valid())
                    {
                        diagnostics.error("event cone extraction produced no clone value",
                                          name());
                        return {false, false, {}};
                    }
                }

                std::vector<OpId> producers(model.values().size() + 1);
                for (const auto &op : model.operations())
                    for (const auto result : model.results(op))
                        producers[result.index] = op.id;
                const ConeInitEval initEval(model, producers);

                uint32_t fallbacks = 0;
                for (const auto &cluster : clusters)
                {
                    const auto &type =
                        model.types()[model.values()[cluster.event.index - 1].type.index - 1];
                    std::string prevInit;
                    if (const auto bits = initEval.eval(cluster.event))
                        prevInit = hexLiteral(type.width, *bits);
                    else
                    {
                        prevInit = hexLiteral(type.width, 0);
                        ++fallbacks;
                    }
                    const std::array<Parameter, 4> params{
                        Parameter{model.intern("edge"), cluster.edge},
                        Parameter{model.intern("act"), cluster.index},
                        Parameter{model.intern("prev"), cluster.index},
                        Parameter{model.intern("prevInit"), std::move(prevInit)}};
                    const auto det = model.addOperation("core.event.edgeDet",
                                                        std::array{cluster.clone}, {}, {}, params);
                    model.setOperationPhase(det, SimPhase::Event);
                }

                // Rewire the consumers to the cluster form: drop the trailing
                // event operands; object refs carry no event slots and stay.
                for (const auto &consumer : consumers)
                {
                    const auto op = model.operations()[consumer.id.index - 1];
                    const auto operands = model.operands(op);
                    const auto refs = model.objectRefs(op);
                    const auto params = model.parameters(op);
                    const std::size_t count = consumer.edges.size();
                    std::vector<ValueId> newOperands(operands.begin(), operands.end() - count);
                    std::vector<ObjectRef> newRefs(refs.begin(), refs.end());
                    std::vector<Parameter> newParams;
                    newParams.reserve(params.size() + 1);
                    for (const auto &parameter : params)
                        if (model.text(parameter.name) != "event_edges")
                            newParams.push_back(parameter);
                    if (count != 0)
                    {
                        std::vector<int64_t> acts;
                        acts.reserve(count);
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            const auto event = operands[operands.size() - count + i];
                            acts.push_back(clusterByKey.at({event.index, consumer.edges[i]}));
                        }
                        newParams.push_back(Parameter{model.intern("event_acts"), std::move(acts)});
                    }
                    const std::vector<ValueId> results(model.results(op).begin(),
                                                       model.results(op).end());
                    const auto opType = std::string(model.text(op.opType));
                    model.replaceOperation(op.id, opType, newOperands, results, newRefs, newParams);
                    // The four mem writes keep phase None for M3 split-phases;
                    // every other consumer joins P_general.
                    model.setOperationPhase(op.id, isMemWriteType(opType) ? SimPhase::None
                                                                          : SimPhase::General);
                }

                // Sweep any unreferenced __event_* history state left by a
                // pre-M5 checkpoint (compact rebuilds the InitRecords), then
                // sweep the dead original cone.
                std::vector<uint8_t> referencedStates(model.states().size() + 1, 0);
                for (const auto &op : model.operations())
                    for (const auto ref : model.objectRefs(op))
                        if (ref.kind == ObjectKind::State) referencedStates[ref.index] = 1;
                std::vector<uint8_t> removeStates(model.states().size() + 1, 0);
                uint32_t removedHistory = 0;
                for (const auto &state : model.states())
                {
                    if (referencedStates[state.id.index] ||
                        !model.text(state.name).starts_with("__event_"))
                        continue;
                    removeStates[state.id.index] = 1;
                    ++removedHistory;
                }
                const auto deadOps = sweepDeadConeOps(model, extraction.coneOps);
                std::vector<uint8_t> removeOps(model.operations().size() + 1, 0);
                for (const auto id : deadOps) removeOps[id.index] = 1;
                model.compact(removeOps, removeStates);

                diagnostics.info("clusters=" + std::to_string(clusters.size()) +
                                     " edge_dets=" + std::to_string(clusters.size()) +
                                     " rewritten_ops=" + std::to_string(consumers.size()) +
                                     " removed_history_states=" + std::to_string(removedHistory) +
                                     " removed_cone_ops=" + std::to_string(deadOps.size()) +
                                     " prev_init_fallbacks=" + std::to_string(fallbacks),
                                 name());
                return {true, true, {}};
            }
        };
    } // namespace

    void registerLowerEdgeDetectPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.lower-edge-detect", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.lower-edge-detect does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<LowerEdgeDetectPass>());
            },
            error);
    }

} // namespace wolvrix::lib::grhsim
