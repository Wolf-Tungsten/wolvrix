#include "grhsim/pass/clone_shared_compute.hpp"
#include "grhsim/ir/model.hpp"

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
                uint32_t candidates = 0, skipped = 0, cloned = 0, dead = 0;
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
                for (std::size_t cursor = 0; cursor < ready.size(); ++cursor)
                {
                    const auto root = ready[cursor];
                    if (const auto parent = parents[root.index]; parent && --children[parent.index] == 0)
                        ready.push_back(parent);
                    // Copy IDs/ranges before appending; operation/value/pool
                    // vectors and the string interner can move during insertion.
                    const auto source = model.operations()[root.index - 1];
                    const auto result = model.results(source)[0];
                    std::vector<OpId> computeUsers;
                    std::size_t consumers = 0;
                    for (auto user : users[result.index])
                    {
                        if (removeOps[user.index]) continue;
                        const auto &op = model.operations()[user.index - 1];
                        const auto operands = model.operands(op);
                        if (std::find(operands.begin(), operands.end(), result) == operands.end()) continue;
                        ++consumers;
                        if (model.text(op.opType).starts_with("core.compute."))
                            computeUsers.push_back(user);
                    }
                    if (computeUsers.empty()) continue;
                    if (consumers < 2 || consumers > options_.maxFanout ||
                        computeUsers.size() > options_.maxClones - cloned)
                    {
                        ++skipped;
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
                    if (consumers == computeUsers.size())
                    {
                        removeOps[root.index] = 1;
                        ++dead;
                    }
                }

                if (cloned != 0)
                {
                    // Non-compute users retain the original producer. No reads,
                    // state objects, effects, or unrelated dead operations vanish.
                    // Repack replaced operand ranges even when a commit user keeps
                    // every root alive; serialized pool counts must stay exact.
                    model.compact(removeOps, std::vector<uint8_t>(model.states().size() + 1));
                }
                diagnostics.info("shared_compute_candidates=" + std::to_string(candidates) +
                                 " cloned=" + std::to_string(cloned) +
                                 " dead_sources_removed=" + std::to_string(dead) +
                                 " skipped=" + std::to_string(skipped) +
                                 " cyclic=" + std::to_string(candidates - ready.size()), name());
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
