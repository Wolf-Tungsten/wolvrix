// V3-M3 (C2.5): cpu.st.clone-shared-boundaries — boundary-aware shared
// compute cloning inside the CPU mapping stage. This is the C-segment
// landing of the removed B7 (grhsim.clone-shared-compute): instead of
// predicting C1 cone boundaries on the semantic layer and cloning against
// the prediction, this pass runs right after C2
// (cpu.st.merge-general-supernodes) where the REAL supernode boundaries are
// known (sixPhaseBoundaryValues on the C2 tree). Every downstream mapping
// pass (C3 layout, C4 activation map, C5 mem plan, C6 function packing, C7
// schedule, C8 TU plan) runs afterwards and simply sees the adjusted model,
// so no mapping invalidation/rebuild is needed — the only payload
// maintained in place is the partition tree:
//
//   * clones (cheap bijective compute ops, one per consuming supernode) are
//     attached into the consuming supernode's child node right before the
//     earliest consumer, preserving the local topological order;
//   * source ops whose consumers all migrated are dropped from the tree and
//     removed via compact(); the pass computes the same dense op-id remap
//     compact() applies and rewrites the tree's op lists to match, and
//     remaps attrs.enableGuard (the A1 sink enable guard, the tree's only
//     value-id payload) with compact()'s dense value renumbering — sink
//     consumers never migrate, so a guard value always survives.
//
// The pass is the sole registered C-segment semantic micro-adjustment: it
// ends with commitSemanticMicroMutation() (revision bump, no mapping wipe)
// plus setCpuMapping() re-stamp, and the post-pass model verify enforces
// total phase attribution and partition coverage as the hard guard rail.

#include "grhsim/backend/cpu.hpp"

#include "grhsim/backend/cpu_phase_common.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/pass/pass.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
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

        // Cheap bijection candidate (the B7 cost model): single-result,
        // parameter/ref-free not/logicNot/xor/add/sub with exactly one
        // varying operand, two-state scalar 1..64 bits. f(x) changes iff x
        // changes, so deleting the boundary compare cannot filter source
        // changes; the varying parent must itself be shared (users > 1) or
        // the clone would only move the boundary to an otherwise local input.
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
            return varying && users[varying.index].size() > 1 ? varying : ValueId{};
        }

        class CloneSharedBoundariesPass final : public Pass
        {
        public:
            explicit CloneSharedBoundariesPass(Options options)
                : Pass("cpu.st.clone-shared-boundaries", PassKind::BackendMapping), options_(options) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::GeneralSupernodes)
                {
                    diagnostics.error("requires cpu.st.merge-general-supernodes output", name());
                    return {false, false, {}};
                }
                CpuBackendMapping mapping = *previous;
                auto &tree = mapping.partitionTree;
                const auto order = generalSupernodeOrder(tree);
                const auto boundary = sixPhaseBoundaryValues(model, tree, generalSupernodeOf(model, tree));
                const auto originalCount = model.operations().size();

                // op -> owning supernode PartitionId / its ordinal / the op's
                // position inside the supernode's flattened op order. All
                // three grow as clones are appended.
                auto supernodeOf = generalSupernodeOf(model, tree);
                std::vector<uint32_t> ordinalOf(tree.partitions.size() + 1, ~0u);
                for (uint32_t i = 0; i < order.size(); ++i) ordinalOf[order[i].index] = i;
                std::vector<std::vector<OpId>> flat(order.size());
                std::vector<uint32_t> position(originalCount + 1, ~0u);
                for (uint32_t ordinal = 0; ordinal < order.size(); ++ordinal)
                    for (const auto node : tree.partitions[order[ordinal].index - 1].children)
                        for (const auto op : tree.partitions[node.index - 1].ops)
                        {
                            position[op.index] = static_cast<uint32_t>(flat[ordinal].size());
                            flat[ordinal].push_back(op);
                        }

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

                // Insert a clone at a flattened position of one supernode,
                // keeping the child node op lists, the ownership table and
                // the position table in sync.
                const auto attachClone = [&](uint32_t ordinal, OpId clone, uint32_t flatPos) {
                    auto &nodeIds = tree.partitions[order[ordinal].index - 1].children;
                    auto &flatOps = flat[ordinal];
                    if (flatPos > flatOps.size()) throw std::logic_error("clone attach position out of range");
                    uint32_t base = 0;
                    for (const auto nodeId : nodeIds)
                    {
                        auto &ops = tree.partitions[nodeId.index - 1].ops;
                        if (flatPos <= base + ops.size())
                        {
                            ops.insert(ops.begin() + (flatPos - base), clone);
                            flatOps.insert(flatOps.begin() + flatPos, clone);
                            supernodeOf.push_back(order[ordinal]);
                            position.push_back(~0u);
                            for (uint32_t i = flatPos; i < flatOps.size(); ++i)
                                position[flatOps[i].index] = i;
                            return;
                        }
                        base += static_cast<uint32_t>(ops.size());
                    }
                    throw std::logic_error("clone attach found no child node");
                };

                std::vector<uint8_t> removeOps(originalCount + 1);
                uint32_t cloned = 0, dead = 0, boundaryHits = 0;
                uint32_t skippedFanout = 0, skippedNonCompute = 0, skippedLocal = 0,
                         skippedPlacement = 0, skippedBudget = 0;
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
                    const auto sourceSupernode = supernodeOf[root.index];
                    // Live consumers, grouped per consuming supernode; local
                    // consumers (same supernode as the source) keep the
                    // source and are not cloned.
                    std::map<uint32_t, std::vector<OpId>> targets;
                    std::size_t consumers = 0, localConsumers = 0;
                    bool nonComputeConsumer = false;
                    for (auto user : users[result.index])
                    {
                        if (user.index >= removeOps.size() || removeOps[user.index]) continue;
                        const auto &op = model.operations()[user.index - 1];
                        const auto operands = model.operands(op);
                        if (std::find(operands.begin(), operands.end(), result) == operands.end()) continue;
                        ++consumers;
                        const auto owner = supernodeOf[user.index];
                        if (owner && owner == sourceSupernode) { ++localConsumers; continue; }
                        // Only same-phase compute consumers inside a non-sink
                        // General supernode move to a clone; anything else
                        // (Mem-phase sampler, sink op, Event/Output branch
                        // consumer, side effect) keeps the boundary alive and
                        // blocks the clone.
                        if (op.phase != sourcePhase ||
                            !model.text(op.opType).starts_with("core.compute.") || !owner ||
                            *tree.partitions[owner.index - 1].attrs.supernodeCategory !=
                                CpuSupernodeCategory::NonSink)
                        {
                            nonComputeConsumer = true;
                            continue;
                        }
                        targets[ordinalOf[owner.index]].push_back(user);
                    }
                    if (consumers == 0) continue;
                    if (consumers < 2 || consumers > options_.maxFanout)
                    {
                        ++skippedFanout;
                        continue;
                    }
                    if (nonComputeConsumer)
                    {
                        ++skippedNonCompute;
                        continue;
                    }
                    if (targets.empty())
                    {
                        ++skippedLocal;
                        continue;
                    }
                    if (result.index >= boundary.size() || !boundary[result.index])
                    {
                        ++skippedLocal;
                        continue;
                    }
                    ++boundaryHits;
                    if (targets.size() > options_.maxClones - cloned)
                    {
                        ++skippedBudget;
                        continue;
                    }
                    // Placement: inside one target supernode the clone must
                    // precede its earliest consumer while following every
                    // locally produced operand. A violation blocks the whole
                    // candidate (conservative; the boundary simply stays).
                    const auto operandList = std::vector<ValueId>(model.operands(source).begin(),
                                                                  model.operands(source).end());
                    bool placementInvalid = false;
                    for (const auto &[ordinal, consumerOps] : targets)
                    {
                        uint32_t earliest = ~0u;
                        for (const auto user : consumerOps) earliest = std::min(earliest, position[user.index]);
                        for (const auto operand : operandList)
                        {
                            const auto producer = producers[operand.index];
                            if (!producer || supernodeOf[producer.index] != order[ordinal]) continue;
                            if (position[producer.index] >= earliest) placementInvalid = true;
                        }
                    }
                    if (placementInvalid)
                    {
                        ++skippedPlacement;
                        continue;
                    }

                    const auto value = model.values()[result.index - 1];
                    const auto opType = std::string(model.text(source.opType));
                    const auto sourceName = std::string(model.text(source.name));
                    for (const auto &[ordinal, consumerOps] : targets)
                    {
                        uint32_t earliest = ~0u;
                        for (const auto user : consumerOps) earliest = std::min(earliest, position[user.index]);
                        const auto cloneName = sourceName + ".local" + std::to_string(ordinal) +
                                               "." + std::to_string(root.index);
                        const auto local = model.addValue(value.type, cloneName, value.origin);
                        const auto clone = model.addOperation(opType, operandList, std::array{local}, {}, {},
                                                              cloneName, source.origin);
                        model.setOperationPhase(clone, sourcePhase);
                        attachClone(ordinal, clone, earliest);
                        removeOps.push_back(0);
                        users.resize(model.values().size() + 1);
                        producers.resize(model.values().size() + 1);
                        producers[local.index] = clone;
                        for (const auto operand : operandList)
                        {
                            auto &list = users[operand.index];
                            if (list.empty() || list.back() != clone) list.push_back(clone);
                        }
                        for (const auto user : consumerOps)
                        {
                            users[local.index].push_back(user);
                            const auto op = model.operations()[user.index - 1];
                            std::vector<ValueId> inputs(model.operands(op).begin(), model.operands(op).end());
                            std::replace(inputs.begin(), inputs.end(), result, local);
                            const std::vector<ValueId> results(model.results(op).begin(), model.results(op).end());
                            const std::vector<ObjectRef> refs(model.objectRefs(op).begin(), model.objectRefs(op).end());
                            const std::vector<Parameter> params(model.parameters(op).begin(), model.parameters(op).end());
                            const auto userType = std::string(model.text(op.opType));
                            model.replaceOperation(op.id, userType, inputs, results, refs, params);
                        }
                        ++cloned;
                    }
                    // The shared source is dead once nothing references its
                    // result anymore: no local consumers kept it and every
                    // crossing consumer moved to a clone.
                    if (localConsumers == 0)
                    {
                        removeOps[root.index] = 1;
                        ++dead;
                    }
                }

                if (cloned != 0)
                {
                    // Dense op-id remap matching compact()'s survivor
                    // renumbering (old survivors first, then the appended
                    // clones, which always survive), applied to the tree
                    // before re-stamping.
                    std::vector<uint32_t> opRemap(model.operations().size() + 1, 0);
                    uint32_t next = 1;
                    for (std::size_t i = 1; i <= originalCount; ++i)
                        if (!removeOps[i]) opRemap[i] = next++;
                    for (std::size_t i = originalCount + 1; i < opRemap.size(); ++i) opRemap[i] = next++;
                    for (auto &partition : tree.partitions)
                    {
                        auto &ops = partition.ops;
                        for (auto &op : ops) op = OpId{opRemap[op.index], 0};
                        ops.erase(std::remove(ops.begin(), ops.end(), OpId{}), ops.end());
                    }
                    // Dense value-id remap matching compact()'s value
                    // renumbering (results of surviving ops, kept in original
                    // value order). attrs.enableGuard (A1) is the partition
                    // tree's only value-id payload and must follow the same
                    // remap; a guard value always survives (its sink
                    // consumers never migrate), so a zero remap is a bug.
                    std::vector<uint32_t> valueRemap(model.values().size() + 1, 0);
                    {
                        std::vector<uint8_t> liveValues(model.values().size() + 1, 0);
                        for (const auto &op : model.operations())
                            if (!removeOps[op.id.index])
                                for (auto value : model.results(op)) liveValues[value.index] = 1;
                        uint32_t nextValue = 1;
                        for (std::size_t i = 1; i < liveValues.size(); ++i)
                            if (liveValues[i]) valueRemap[i] = nextValue++;
                    }
                    for (auto &partition : tree.partitions)
                        if (partition.attrs.enableGuard)
                        {
                            const auto remapped =
                                valueRemap[static_cast<std::size_t>(*partition.attrs.enableGuard)];
                            if (remapped == 0)
                                throw std::runtime_error("clone-shared-boundaries removed an enable guard value");
                            partition.attrs.enableGuard = static_cast<int64_t>(remapped);
                        }
                    // Nodes whose ops all died (a removed source was their
                    // only op) and supernodes left with no node must leave
                    // the tree: the verifier requires nonempty leaves.
                    // Partition ids only live inside the tree at this stage,
                    // so a dense renumber with parent/children remap is safe.
                    {
                        PartitionId generalId;
                        for (const auto child : tree.partitions[tree.root.index - 1].children)
                            if (tree.partitions[child.index - 1].attrs.phase == CpuPhase::General)
                                generalId = child;
                        auto &general = tree.partitions[generalId.index - 1];
                        std::vector<char> drop(tree.partitions.size() + 1, 0);
                        std::vector<PartitionId> supernodes;
                        for (const auto child : general.children)
                        {
                            auto &supernode = tree.partitions[child.index - 1];
                            if (supernode.attrs.kind != CpuPartitionKind::Supernode)
                            {
                                supernodes.push_back(child);
                                continue;
                            }
                            std::vector<PartitionId> nodes;
                            for (const auto node : supernode.children)
                            {
                                if (tree.partitions[node.index - 1].ops.empty()) drop[node.index] = 1;
                                else nodes.push_back(node);
                            }
                            supernode.children = nodes;
                            if (nodes.empty()) drop[child.index] = 1;
                            else supernodes.push_back(child);
                        }
                        general.children = std::move(supernodes);
                        if (std::any_of(drop.begin(), drop.end(), [](char c) { return c != 0; }))
                        {
                            std::vector<uint32_t> partitionRemap(tree.partitions.size() + 1, 0);
                            std::vector<CpuPartition> partitions;
                            partitions.reserve(tree.partitions.size());
                            for (const auto &partition : tree.partitions)
                                if (!drop[partition.id.index])
                                {
                                    partitionRemap[partition.id.index] =
                                        static_cast<uint32_t>(partitions.size()) + 1;
                                    partitions.push_back(partition);
                                }
                            for (auto &partition : partitions)
                            {
                                partition.id = PartitionId{partitionRemap[partition.id.index], 0};
                                if (partition.parent)
                                    partition.parent = PartitionId{partitionRemap[partition.parent.index], 0};
                                for (auto &child : partition.children)
                                    child = PartitionId{partitionRemap[child.index], 0};
                            }
                            tree.partitions = std::move(partitions);
                            tree.root = tree.partitions.front().id;
                        }
                    }
                    model.compact(removeOps, std::vector<uint8_t>(model.states().size() + 1));
                    model.commitSemanticMicroMutation();
                    model.setCpuMapping(std::move(mapping));
                }
                const std::string idleReason = cloned != 0 ? "none" :
                    candidates == 0 ? "no_candidates" :
                    boundaryHits == 0 ? "no_boundary_candidates" :
                    skippedBudget != 0 ? "budget" : "gated";
                diagnostics.info("clone_shared_candidates=" + std::to_string(candidates) +
                                 " boundary_hits=" + std::to_string(boundaryHits) +
                                 " cloned=" + std::to_string(cloned) +
                                 " boundary_values_eliminated=" + std::to_string(dead) +
                                 " dead_sources_removed=" + std::to_string(dead) +
                                 " skipped_fanout=" + std::to_string(skippedFanout) +
                                 " skipped_non_compute_consumer=" + std::to_string(skippedNonCompute) +
                                 " skipped_local=" + std::to_string(skippedLocal) +
                                 " skipped_placement=" + std::to_string(skippedPlacement) +
                                 " skipped_budget=" + std::to_string(skippedBudget) +
                                 " cyclic=" + std::to_string(candidates - ready.size()) +
                                 " idle_reason=" + idleReason, name());
                return {true, cloned != 0, {}};
            }

        private:
            Options options_;
        };

        bool parsePositive(std::string_view value, uint32_t &target)
        {
            uint32_t parsed = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            return result.ec == std::errc() && result.ptr == value.data() + value.size() && parsed > 0 &&
                   (target = parsed, true);
        }
    }

    void registerCpuCloneSharedPasses(PassRegistry &registry)
    {
        std::string error;
        if (!registry.registerPass("cpu.st.clone-shared-boundaries", PassKind::BackendMapping,
                                   [](std::span<const std::string_view> args, std::string &error)
                                       -> std::unique_ptr<Pass> {
                                       constexpr std::string_view usage = "expected [--max-fanout <n>] [--max-clones <n>]";
                                       if (args.size() % 2 != 0) { error = std::string(usage); return {}; }
                                       Options options;
                                       const std::array<std::string_view, 2> keys{"--max-fanout", "--max-clones"};
                                       std::vector<bool> seen(keys.size());
                                       for (std::size_t i = 0; i < args.size(); i += 2)
                                       {
                                           const auto it = std::find(keys.begin(), keys.end(), args[i]);
                                           if (it == keys.end()) { error = std::string(usage); return {}; }
                                           const auto index = static_cast<std::size_t>(it - keys.begin());
                                           if (seen[index]) { error = std::string(usage); return {}; }
                                           seen[index] = true;
                                           uint32_t *target = index == 0 ? &options.maxFanout : &options.maxClones;
                                           if (!parsePositive(args[i + 1], *target)) { error = std::string(usage); return {}; }
                                       }
                                       return std::make_unique<CloneSharedBoundariesPass>(options);
                                   },
                                   error))
            throw std::logic_error(error);
    }
}
