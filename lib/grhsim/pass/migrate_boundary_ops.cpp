#include "grhsim/pass/migrate_boundary_ops.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim {
    namespace {
        // Migrate single-consumer pure compute ops out of their producer compute
        // supernode into the consumer compute supernode (NO00013). A boundary
        // value v produced by op X in unit A whose consumers are all compute ops
        // in one other unit B stops being a boundary value: its boundary store,
        // change detection and fanout arming disappear. Eligibility is fully
        // static (dependency-driven): every operand of X must already be
        // available in B — produced inside B, already read by B cross-unit, or a
        // core.compute.constant (inlined at read sites by the emitter). The one
        // exception: a constant result that is local to the donor unit is not
        // inlined by the data layout, so migrating its last cross-unit reader
        // would turn it into a new boundary value; such anchors are rejected
        // (constants themselves never migrate). With zero new fanout/input
        // edges B's firing set is provably unchanged (v-change events are a
        // subset of X's-operand-change events, which all already trigger B),
        // and A's firing set can only shrink.
        //
        // Runs on the complete CPU schedule mapping (PassKind::BackendMapping):
        // only partition-tree op lists and helperChunks are edited; the data
        // layout and schedule are canonical functions of the tree and are
        // rebuilt via refreshCpuDataLayout / refreshCpuSchedule. The emitter is
        // untouched: with v no longer boundary and without fanout, its def
        // degenerates to a plain local store inside B.
        //
        // A migrated op's result consumers always stay in B: if one of them
        // migrated out, its operand v would have to be available in that op's own
        // target unit, which the single-consumer-unit rule forbids. Migrants into
        // one supernode are therefore always independent of each other and of
        // donor-side removals, so each one inserts immediately before its
        // earliest original consumer in B's flattened order (topologically legal:
        // every operand producer precedes X, hence precedes that consumer).
        class MigrateBoundaryOpsPass final : public Pass {
        public:
            MigrateBoundaryOpsPass()
                : Pass("grhsim.migrate-boundary-ops", PassKind::BackendMapping) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::Schedule ||
                    !previous->dataLayout || !previous->schedule)
                    throw std::runtime_error(
                        "grhsim.migrate-boundary-ops requires a complete CPU schedule mapping");
                const auto &operations = model.operations();
                for (const auto &op : operations)
                    if (model.text(op.opType) == "core.compute.expr")
                        throw std::runtime_error(
                            "grhsim.migrate-boundary-ops does not support models containing core.compute.expr");

                CpuBackendMapping mapping = *previous;
                auto &tree = mapping.partitionTree;
                const std::size_t valueCount = model.values().size();
                const std::size_t opCount = operations.size();

                std::vector<uint32_t> producer(valueCount + 1, 0);
                std::vector<std::vector<uint32_t>> consumers(valueCount + 1);
                for (const auto &op : operations)
                    for (auto result : model.results(op)) producer[result.index] = op.id.index;
                for (const auto &op : operations)
                    for (auto operand : model.operands(op)) consumers[operand.index].push_back(op.id.index);

                // Compute supernodes (supernode partitions with node children)
                // with flattened op order and per-position node ordinals.
                struct Supernode {
                    uint32_t part = 0;              // partition index (1-based)
                    std::vector<uint32_t> nodes;    // child node partition indices
                    std::vector<uint32_t> flat;     // flattened op ids
                    std::vector<uint32_t> nodeOf;   // node ordinal per flat position
                };
                std::vector<Supernode> supernodes;
                std::vector<uint32_t> owner(opCount + 1, 0);
                for (uint32_t part = 1; part <= tree.partitions.size(); ++part) {
                    const auto &partition = tree.partitions[part - 1];
                    if (partition.attrs.kind != CpuPartitionKind::Supernode || partition.children.empty())
                        continue;
                    const auto &first = tree.partitions[partition.children.front().index - 1];
                    if (first.attrs.kind != CpuPartitionKind::Node) continue;  // commit leaf
                    Supernode sn;
                    sn.part = part;
                    for (auto child : partition.children) {
                        const auto &node = tree.partitions[child.index - 1];
                        sn.nodes.push_back(child.index);
                        for (auto opId : node.ops) {
                            sn.flat.push_back(opId.index);
                            sn.nodeOf.push_back(static_cast<uint32_t>(sn.nodes.size() - 1));
                            owner[opId.index] = part;
                        }
                    }
                    supernodes.push_back(std::move(sn));
                }

                // Cross-unit operand values per supernode: the values whose change
                // already triggers the unit's activation.
                std::unordered_map<uint32_t, std::unordered_set<uint32_t>> unitInputs;
                for (const auto &sn : supernodes) {
                    auto &seen = unitInputs[sn.part];
                    for (auto opIdx : sn.flat)
                        for (auto value : model.operands(operations[opIdx - 1]))
                            if (owner[producer[value.index]] != sn.part) seen.insert(value.index);
                }

                // Values that must keep their boundary slot.
                std::vector<uint8_t> pinned(valueCount + 1, 0);
                for (const auto &slot : mapping.dataLayout->runtime)
                    if (slot.value) pinned[slot.value.index] = 1;
                for (const auto &shadow : mapping.schedule->inputShadows) pinned[shadow.value.index] = 1;
                for (const auto &row : mapping.schedule->inputFanout) pinned[row.source.index] = 1;
                for (const auto &partition : tree.partitions)
                    if (partition.attrs.eventGate)
                        for (const auto &event : partition.attrs.eventGate->events)
                            pinned[event.value.index] = 1;

                const auto &layout = *mapping.dataLayout;
                const auto constantValue = [&](uint32_t value) {
                    const uint32_t prod = producer[value];
                    return prod != 0 && model.text(operations[prod - 1].opType) == "core.compute.constant";
                };

                struct Move { uint32_t op, value, from, to; };
                std::vector<Move> moves;
                std::vector<uint32_t> donorRemovals(tree.partitions.size() + 1, 0);
                for (const auto &sn : supernodes) {
                    for (auto opIdx : sn.flat) {
                        const auto &op = operations[opIdx - 1];
                        const std::string_view name = model.text(op.opType);
                        if (name.size() <= 13 || name.compare(0, 13, "core.compute.") != 0 ||
                            name == "core.compute.constant")
                            continue;
                        const auto results = model.results(op);
                        if (results.size() != 1 || !model.objectRefs(op).empty()) continue;
                        const uint32_t value = results[0].index;
                        if (layout.values[value - 1].kind != CpuStorageKind::Boundary || pinned[value])
                            continue;
                        const auto &type = model.types()[model.values()[value - 1].type.index - 1];
                        if (type.kind != TypeKind::Logic || type.domain != LogicDomain::TwoState ||
                            type.width < 1 || type.width > 64)
                            continue;
                        uint32_t target = 0;
                        bool eligible = true;
                        for (auto consumer : consumers[value]) {
                            const uint32_t unit = owner[consumer];
                            if (unit == 0 || unit == sn.part || (target != 0 && unit != target)) {
                                eligible = false;
                                break;
                            }
                            target = unit;
                        }
                        if (!eligible || target == 0) continue;
                        const auto &inputs = unitInputs[target];
                        for (auto operand : model.operands(op)) {
                            const uint32_t w = operand.index;
                            if (owner[producer[w]] == target || inputs.count(w))
                                continue;
                            if (constantValue(w)) {
                                // A donor-local constant result is not inlined by
                                // the layout: once X leaves, it would become a new
                                // boundary value (constants never co-migrate).
                                const uint32_t prod = producer[w];
                                if (owner[prod] == sn.part) {
                                    bool localOnly = true;
                                    for (auto cons : consumers[w]) {
                                        if (cons != opIdx && owner[cons] != sn.part) {
                                            localOnly = false;
                                            break;
                                        }
                                    }
                                    if (localOnly) {
                                        eligible = false;
                                        break;
                                    }
                                }
                                continue;
                            }
                            eligible = false;
                            break;
                        }
                        if (!eligible) continue;
                        moves.push_back({opIdx, value, sn.part, target});
                        ++donorRemovals[sn.part];
                    }
                }

                // A supernode must keep at least one op: active words pack eight
                // aligned consecutive supernodes, so emptying one would force a
                // full tree repack. Per donor, drop the last flat-order candidates
                // that would empty it (moves are collected in donor flat order).
                uint64_t restored = 0;
                {
                    std::vector<uint32_t> flatSize(tree.partitions.size() + 1, 0);
                    for (const auto &sn : supernodes) flatSize[sn.part] = sn.flat.size();
                    std::vector<uint8_t> spared(tree.partitions.size() + 1, 0);
                    std::size_t kept = moves.size();
                    for (auto it = moves.end(); it != moves.begin();) {
                        --it;
                        if (donorRemovals[it->from] < flatSize[it->from] || spared[it->from]) continue;
                        spared[it->from] = 1;
                        --donorRemovals[it->from];
                        ++restored;
                        --kept;
                        it->op = 0;  // mark dropped
                    }
                    moves.erase(std::remove_if(moves.begin(), moves.end(),
                                               [](const Move &m) { return m.op == 0; }),
                                moves.end());
                    (void)kept;
                }
                if (moves.empty()) {
                    diagnostics.info("migrate_boundary_ops=0 restored=" + std::to_string(restored), name());
                    return {true, false, {}};
                }

                std::unordered_map<uint32_t, std::vector<const Move *>> movesOut, movesIn;
                for (const auto &move : moves) {
                    movesOut[move.from].push_back(&move);
                    movesIn[move.to].push_back(&move);
                }

                uint64_t droppedNodes = 0;
                bool renumber = false;
                for (auto &sn : supernodes) {
                    const auto outIt = movesOut.find(sn.part);
                    const auto inIt = movesIn.find(sn.part);
                    if (outIt == movesOut.end() && inIt == movesIn.end()) continue;

                    // Post-removal slots, keeping node/chunk ordinals per position.
                    std::vector<uint8_t> removed(sn.flat.size(), 0);
                    if (outIt != movesOut.end()) {
                        std::vector<uint32_t> position(opCount + 1, UINT32_MAX);
                        for (uint32_t i = 0; i < sn.flat.size(); ++i) position[sn.flat[i]] = i;
                        for (const auto *move : outIt->second) removed[position[move->op]] = 1;
                    }
                    struct Slot { uint32_t op, node, chunk; };
                    std::vector<Slot> slots;
                    {
                        const auto &chunks = tree.partitions[sn.part - 1].attrs.helperChunks;
                        uint32_t chunk = 0;
                        for (uint32_t i = 0; i < sn.flat.size(); ++i) {
                            if (removed[i]) continue;
                            while (!chunks.empty() && chunk + 1 < chunks.size() &&
                                   i >= chunks[chunk].offset + chunks[chunk].count)
                                ++chunk;
                            slots.push_back({sn.flat[i], sn.nodeOf[i], chunks.empty() ? 0 : chunk});
                        }
                    }
                    // Insert each inbound migrant immediately before its earliest
                    // consumer currently present (consumers never migrate out, see
                    // the header note). Migrants are mutually independent.
                    if (inIt != movesIn.end()) {
                        auto inbound = inIt->second;
                        std::sort(inbound.begin(), inbound.end(),
                                  [](const Move *a, const Move *b) { return a->op < b->op; });
                        for (const auto *move : inbound) {
                            bool found = false;
                            for (uint32_t i = 0; i < slots.size(); ++i) {
                                bool readsValue = false;
                                for (auto operand : model.operands(operations[slots[i].op - 1]))
                                    if (operand.index == move->value) { readsValue = true; break; }
                                if (!readsValue) continue;
                                slots.insert(slots.begin() + i, {move->op, slots[i].node, slots[i].chunk});
                                found = true;
                                break;
                            }
                            if (!found)
                                throw std::runtime_error(
                                    "grhsim.migrate-boundary-ops lost a consumer of a migrated value");
                        }
                    }

                    // Rebuild node op lists and helper chunks from slot ordinals.
                    auto &partition = tree.partitions[sn.part - 1];
                    std::vector<std::vector<OpId>> nodeOps(sn.nodes.size());
                    std::vector<uint32_t> chunkCount;
                    for (const auto &slot : slots) {
                        nodeOps[slot.node].push_back(OpId{slot.op, 0});
                        if (chunkCount.size() <= slot.chunk) chunkCount.resize(slot.chunk + 1, 0);
                        ++chunkCount[slot.chunk];
                    }
                    partition.children.clear();
                    for (uint32_t k = 0; k < sn.nodes.size(); ++k) {
                        if (nodeOps[k].empty()) {
                            tree.partitions[sn.nodes[k] - 1].ops.clear();
                            ++droppedNodes;
                            renumber = true;
                            continue;
                        }
                        auto &nodePart = tree.partitions[sn.nodes[k] - 1];
                        nodePart.ops = std::move(nodeOps[k]);
                        partition.children.push_back(nodePart.id);
                    }
                    if (partition.children.empty())
                        throw std::runtime_error(
                            "grhsim.migrate-boundary-ops emptied a compute supernode");
                    auto &chunks = partition.attrs.helperChunks;
                    if (!chunks.empty()) {
                        std::vector<Range> rebuilt;
                        uint32_t offset = 0;
                        for (uint32_t count : chunkCount) {
                            if (count == 0) continue;
                            rebuilt.push_back(Range{offset, count});
                            offset += count;
                        }
                        if (rebuilt.empty() || offset != slots.size())
                            throw std::runtime_error(
                                "grhsim.migrate-boundary-ops helper chunk rebuild lost coverage");
                        chunks = std::move(rebuilt);
                    }
                }

                if (renumber) {
                    // Drop removed (emptied) node partitions and renumber densely.
                    std::vector<uint32_t> remap(tree.partitions.size() + 1, 0);
                    std::vector<CpuPartition> surviving;
                    surviving.reserve(tree.partitions.size());
                    for (uint32_t part = 1; part <= tree.partitions.size(); ++part) {
                        const auto &partition = tree.partitions[part - 1];
                        if (partition.attrs.kind == CpuPartitionKind::Node && partition.ops.empty())
                            continue;
                        remap[part] = static_cast<uint32_t>(surviving.size()) + 1;
                        surviving.push_back(partition);
                    }
                    for (auto &partition : surviving) {
                        partition.id = PartitionId{remap[partition.id.index], partition.id.generation};
                        partition.parent = PartitionId{remap[partition.parent.index], partition.parent.generation};
                        for (auto &child : partition.children)
                            child = PartitionId{remap[child.index], child.generation};
                    }
                    tree.root = PartitionId{remap[tree.root.index], tree.root.generation};
                    tree.partitions = std::move(surviving);
                }

                model.setCpuMapping(std::move(mapping));
                if (!refreshCpuDataLayout(model, diagnostics)) return {false, true, {}};
                if (!refreshCpuSchedule(model, diagnostics)) return {false, true, {}};
                diagnostics.info("migrate_boundary_ops=" + std::to_string(moves.size()) +
                    " donors=" + std::to_string(movesOut.size()) +
                    " targets=" + std::to_string(movesIn.size()) +
                    " dropped_nodes=" + std::to_string(droppedNodes) +
                    " restored=" + std::to_string(restored), name());
                return {true, true, {}};
            }
        };
    }

    void registerMigrateBoundaryOpsPass(PassRegistry &registry) {
        std::string error;
        if (!registry.registerPass("grhsim.migrate-boundary-ops", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                if (!args.empty()) {
                    factoryError = "grhsim.migrate-boundary-ops does not accept arguments";
                    return {};
                }
                return std::make_unique<MigrateBoundaryOpsPass>();
            }, error))
            throw std::logic_error(error);
    }
}
