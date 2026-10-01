// grhsim.clone-shared-compute (B7, boundary-aware rework per plan 归位决议 1,
// M5d-5): clones cheap shared bijection producers per compute consumer when —
// and only when — the clone eliminates a predicted P_general supernode
// boundary. The pass reduces supernode boundaries (boundaryValueStore slots,
// change detection and activation notifications), not partition boundaries:
// the Event/Output cones are self-contained, so no cross-partition shared
// compute exists.
//
// Two hard constraints pin the pass to the end of the semantic layer: it is
// a semantic rewrite, so it must run before the single final CPU mapping (no
// "mapping -> semantic rewrite -> mapping" round trip), and it must follow
// the last CSE-bearing simplify (B6, scope=phase) or the clones would be
// re-merged.
//
// Cost model: the candidate definition is unchanged (scalar two-state
// bijection producers — not / 1-bit logicNot / xor / add / sub with exactly
// one constant operand — whose varying input is itself shared, so deleting
// the shared instance cannot merely move the boundary to a local input).
// The blind "fanout >= 2" trigger is replaced by boundary awareness: the
// predicted General node boundaries come from predictGeneralBoundaries
// (general_boundaries.hpp), the helper that statically simulates
// cpu.st.build-general-nodes' cone absorption and is shared with C1. A
// candidate is cloned only when its result is a predicted boundary AND every
// live consumer is a same-phase core.compute.* op — moving all consumers
// onto local clones eliminates the boundary value outright (a survivor
// consumer, e.g. a Mem-phase write sampling the value, would keep the
// boundary and the clone would buy nothing). DPI calls, random sampling and
// side-effecting system tasks are never cloned: they are not compute ops,
// so they neither qualify as candidates nor move to clones.
//
// Clones inherit the source op's SimPhase. On a model without phase
// attribution (grhsim.split-phases has not run) the predicted General op set
// is empty and the pass is a no-op; the idle reason and the boundary
// reduction counters are reported either way.

#include "grhsim/pass/clone_shared_compute.hpp"

#include "grhsim/ir/model.hpp"
#include "grhsim/pass/general_boundaries.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        struct Options
        {
            uint32_t maxFanout = 8;
            uint32_t maxClones = 250000;
            uint32_t maxOpsPerNode = 128;
        };

        const Type &valueType(const GrhSimModel &model, ValueId value)
        {
            return model.types()[model.values()[value.index - 1].type.index - 1];
        }

        bool scalarTwoState(const GrhSimModel &model, ValueId value)
        {
            const auto &type = valueType(model, value);
            return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                   type.width != 0 && type.width <= 64;
        }

        ValueId varyingSource(const GrhSimModel &model, const SimOp &op,
                              const std::vector<OpId> &producers,
                              const std::vector<std::vector<OpId>> &users)
        {
            const auto name = model.text(op.opType);
            if (model.results(op).size() != 1 ||
                !model.objectRefs(op).empty() || !model.parameters(op).empty()) return {};
            const auto result = model.results(op)[0];
            if (!scalarTwoState(model, result)) return {};
            const auto type = model.values()[result.index - 1].type;
            const auto sameType = [&](ValueId value) { return model.values()[value.index - 1].type == type; };
            const auto operands = model.operands(op);
            ValueId varying;
            if (name == "core.compute.not" && operands.size() == 1 && sameType(operands[0]))
                varying = operands[0];
            else if (name == "core.compute.logicNot" && operands.size() == 1 &&
                     scalarTwoState(model, operands[0]) && valueType(model, operands[0]).width == 1 &&
                     valueType(model, result).width == 1)
                varying = operands[0];
            else if ((name == "core.compute.xor" || name == "core.compute.add" || name == "core.compute.sub") &&
                     operands.size() == 2 && sameType(operands[0]) && sameType(operands[1]))
            {
                const auto constant = [&](ValueId value) {
                    if (!producers[value.index]) return false;
                    const auto &source = model.operations()[producers[value.index].index - 1];
                    return model.text(source.opType) == "core.compute.constant" &&
                           model.operands(source).empty() && model.objectRefs(source).empty();
                };
                const bool left = constant(operands[0]), right = constant(operands[1]);
                if (left != right) varying = operands[left ? 1 : 0];
            }
            // These functions are bijections on the normalized scalar bit pattern:
            // f(x) changes iff x changes. The deleted comparison cannot filter any
            // source changes. A shared source avoids merely moving the boundary
            // from the result to an otherwise local input.
            return varying && users[varying.index].size() > 1 ? varying : ValueId{};
        }

        class CloneSharedComputePass final : public Pass
        {
        public:
            explicit CloneSharedComputePass(Options options)
                : Pass("grhsim.clone-shared-compute", PassKind::SemanticTransform), options_(options) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto originalCount = model.operations().size();
                const GeneralBoundaryPrediction prediction =
                    predictGeneralBoundaries(model, options_.maxOpsPerNode);
                uint64_t predictedBoundaries = 0;
                for (const auto marked : prediction.boundaryValue) predictedBoundaries += marked;

                std::vector<OpId> producers(model.values().size() + 1);
                std::vector<std::vector<OpId>> users(model.values().size() + 1);
                for (const auto &op : model.operations())
                {
                    for (auto value : model.results(op)) producers[value.index] = op.id;
                    for (auto value : model.operands(op))
                    {
                        auto &list = users[value.index];
                        // Multiple operand positions in one consumer share one clone.
                        if (list.empty() || list.back() != op.id) list.push_back(op.id);
                    }
                }

                // Each bijection has at most one varying candidate parent. Visit
                // downstream roots first so their clones become the live users
                // of upstream roots, independent of operation insertion order.
                std::vector<ValueId> sources(originalCount + 1);
                uint32_t candidates = 0;
                for (std::size_t i = 0; i < originalCount; ++i)
                {
                    const auto &source = model.operations()[i];
                    const auto varying = varyingSource(model, source, producers, users);
                    if (!varying) continue;
                    const auto result = model.results(source)[0];
                    const auto &consumers = users[result.index];
                    if (consumers.size() < 2 || consumers.size() > options_.maxFanout) continue;
                    if (std::none_of(consumers.begin(), consumers.end(), [&](OpId user) {
                        return model.text(model.operations()[user.index - 1].opType).starts_with("core.compute.");
                    })) continue;
                    sources[source.id.index] = varying;
                    ++candidates;
                }
                std::vector<OpId> parents(originalCount + 1), ready;
                std::vector<uint32_t> children(originalCount + 1);
                for (const auto &source : model.operations())
                    if (const auto varying = sources[source.id.index])
                        if (const auto parent = producers[varying.index]; parent && sources[parent.index])
                        {
                            parents[source.id.index] = parent;
                            ++children[parent.index];
                        }
                for (const auto &source : model.operations())
                    if (sources[source.id.index] && children[source.id.index] == 0) ready.push_back(source.id);

                std::vector<uint8_t> removeOps(originalCount + 1);
                uint32_t cloned = 0, dead = 0, boundaryHits = 0;
                uint32_t skippedFanout = 0, skippedNonCompute = 0, skippedLocal = 0, skippedBudget = 0;
                for (std::size_t cursor = 0; cursor < ready.size(); ++cursor)
                {
                    const auto root = ready[cursor];
                    if (const auto parent = parents[root.index]; parent && --children[parent.index] == 0)
                        ready.push_back(parent);
                    // Copy IDs/ranges before appending; operation/value/pool
                    // vectors and the string interner can move during insertion.
                    const auto source = model.operations()[root.index - 1];
                    const auto sourcePhase = source.phase;
                    const auto result = model.results(source)[0];
                    std::vector<OpId> computeUsers;
                    std::size_t consumers = 0;
                    bool nonComputeConsumer = false;
                    for (auto user : users[result.index])
                    {
                        if (removeOps[user.index]) continue;
                        const auto &op = model.operations()[user.index - 1];
                        const auto operands = model.operands(op);
                        if (std::find(operands.begin(), operands.end(), result) == operands.end()) continue;
                        ++consumers;
                        // Only same-phase compute consumers move to a clone;
                        // anything else (a Mem-phase write sampling the value,
                        // a read, a side-effecting op, a cross-phase consumer)
                        // keeps the boundary alive and blocks the clone.
                        if (op.phase == sourcePhase &&
                            model.text(op.opType).starts_with("core.compute."))
                            computeUsers.push_back(user);
                        else nonComputeConsumer = true;
                    }
                    if (consumers == 0) continue;
                    if (consumers < 2 || consumers > options_.maxFanout)
                    {
                        ++skippedFanout;
                        continue;
                    }
                    if (nonComputeConsumer || computeUsers.size() != consumers)
                    {
                        ++skippedNonCompute;
                        continue;
                    }
                    if (result.index >= prediction.boundaryValue.size() ||
                        !prediction.boundaryValue[result.index])
                    {
                        ++skippedLocal;
                        continue;
                    }
                    ++boundaryHits;
                    if (computeUsers.size() > options_.maxClones - cloned)
                    {
                        ++skippedBudget;
                        continue;
                    }

                    const std::vector<ValueId> operands(model.operands(source).begin(), model.operands(source).end());
                    const auto value = model.values()[result.index - 1];
                    const auto opType = std::string(model.text(source.opType));
                    const auto sourceName = std::string(model.text(source.name));
                    for (auto user : computeUsers)
                    {
                        const auto name = sourceName + ".local" + std::to_string(user.index) +
                                          "." + std::to_string(source.id.index);
                        const auto local = model.addValue(value.type, name, value.origin);
                        const auto clone = model.addOperation(opType, operands, std::array{local}, {}, {}, name, source.origin);
                        // Clones stay in the source's partition: they are
                        // absorbed into the consumer's predicted node.
                        model.setOperationPhase(clone, sourcePhase);
                        removeOps.push_back(0);
                        users.resize(model.values().size() + 1);
                        for (auto operand : operands)
                        {
                            auto &list = users[operand.index];
                            if (list.empty() || list.back() != clone) list.push_back(clone);
                        }
                        users[local.index].push_back(user);
                        const auto op = model.operations()[user.index - 1];
                        std::vector<ValueId> inputs(model.operands(op).begin(), model.operands(op).end());
                        std::replace(inputs.begin(), inputs.end(), result, local);
                        const std::vector<ValueId> results(model.results(op).begin(), model.results(op).end());
                        const std::vector<ObjectRef> refs(model.objectRefs(op).begin(), model.objectRefs(op).end());
                        const std::vector<Parameter> params(model.parameters(op).begin(), model.parameters(op).end());
                        const auto userType = std::string(model.text(op.opType));
                        model.replaceOperation(op.id, userType, inputs, results, refs, params);
                        ++cloned;
                    }
                    // Every consumer moved to a clone: the shared source is
                    // dead and its predicted boundary is eliminated.
                    removeOps[root.index] = 1;
                    ++dead;
                }

                if (cloned != 0)
                {
                    // The strict consumer rule moved every consumer to a
                    // clone, so every visited root is removed; no reads, state
                    // objects, effects, or unrelated dead operations vanish.
                    // Repack replaced operand ranges; serialized pool counts
                    // must stay exact.
                    model.compact(removeOps, std::vector<uint8_t>(model.states().size() + 1));
                }
                std::string idleReason = "none";
                if (cloned == 0)
                {
                    if (prediction.nodeCount == 0)
                        idleReason = "no_general_ops";
                    else if (candidates == 0)
                        idleReason = "no_candidates";
                    else if (boundaryHits == 0)
                        idleReason = "no_boundary_candidates";
                    else
                        idleReason = "budget_exhausted";
                }
                diagnostics.info("shared_compute_candidates=" + std::to_string(candidates) +
                                 " boundary_values_predicted=" + std::to_string(predictedBoundaries) +
                                 " boundary_hits=" + std::to_string(boundaryHits) +
                                 " cloned=" + std::to_string(cloned) +
                                 " boundary_values_eliminated=" + std::to_string(dead) +
                                 " dead_sources_removed=" + std::to_string(dead) +
                                 " skipped_fanout=" + std::to_string(skippedFanout) +
                                 " skipped_non_compute_consumer=" + std::to_string(skippedNonCompute) +
                                 " skipped_local=" + std::to_string(skippedLocal) +
                                 " skipped_budget=" + std::to_string(skippedBudget) +
                                 " cyclic=" + std::to_string(candidates - ready.size()) +
                                 " idle_reason=" + idleReason, name());
                return {true, cloned != 0, {}};
            }

        private:
            Options options_;
        };

        bool parsePositive(std::string_view value, uint32_t &result)
        {
            uint32_t parsed = 0;
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || parsed == 0) return false;
            result = parsed;
            return true;
        }
    }

    void registerCloneSharedComputePass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.clone-shared-compute", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                Options options;
                if (args.size() % 2 != 0)
                {
                    factoryError = "expected option/value pairs";
                    return std::unique_ptr<Pass>{};
                }
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    uint32_t value = 0;
                    if (!parsePositive(args[i + 1], value))
                    {
                        factoryError = "option requires a positive integer: " + std::string(args[i]);
                        return std::unique_ptr<Pass>{};
                    }
                    if (args[i] == "--max-fanout") options.maxFanout = value;
                    else if (args[i] == "--max-clones") options.maxClones = value;
                    else if (args[i] == "--max-op-in-compute-node") options.maxOpsPerNode = value;
                    else
                    {
                        factoryError = "unknown option: " + std::string(args[i]);
                        return std::unique_ptr<Pass>{};
                    }
                }
                return std::unique_ptr<Pass>(std::make_unique<CloneSharedComputePass>(options));
            }, error);
    }
}
