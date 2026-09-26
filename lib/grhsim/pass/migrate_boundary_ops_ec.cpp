#include "grhsim/pass/migrate_boundary_ops_ec.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::grhsim {
    namespace {
        // Edge-completion boundary-op migration (NO00019): migrate a pure
        // compute op X (result v, unit A) into its single consumer compute
        // supernode B even when some operands of X are not yet available in B,
        // as long as each missing operand w is a monitored boundary value with
        // a runtime-live fanout row. Migrating X makes B a reader of w, so the
        // schedule rebuild extends activate(fanout(w)) with B -- the new edge
        // is created by the migration itself; no separate edge machinery is
        // needed. NO00013's five-point equivalence argument is preserved:
        // (1) init() fires every unit once in flattened topological order;
        // (2) within a round, a w change fires w's producer and B (new edge)
        // in the same round, and producer(w) < A (A read w) < B (B reads v)
        // keeps the activation graph acyclic, so X re-evaluates on fresh
        // operands; (3) B's firing set only grows (widen, bounded by Sigma
        // ch_w) and the extra fires are unobservable because B is required
        // side-effect free; (4) A's firing set can only shrink; (5) two-state
        // pure compute is bit deterministic.
        //
        // Safety is fully static (dependency-driven); only the selection is
        // priced with a dynamic per-value profile of the same workload:
        //   profit = wr(v)*(K_DETECT+K_STORE)                       (row gone)
        //          - Sigma_missing ch(w)*((ops_B+1)*K_OP + K_SET)   (extra fires)
        //          - body_B*K_EVAL(kind_X)                          (X re-eval)
        // in exact integer arithmetic scaled by 4 (mirrors
        // grhsim_migrate_ec_census.py bit for bit). A selected value's
        // non-constant operands must not be selected (a migrated operand loses
        // its row / becomes unit-local), so the move set is the greatest
        // fixpoint over the profitable set; the NO00013 donor restore rule is
        // applied afterwards (a supernode keeps at least one op).
        //
        // De-monitor closure (pipeline-position safety): this pass runs after
        // grhsim.demonitor-edge-completion, whose stored removal list is
        // re-validated against the current tree on every schedule rebuild and
        // whose NO00014 companion rule is recomputed from scratch. A value
        // whose fanout row covers a de-monitored value (cover source or
        // completion-edge source) must therefore neither migrate nor serve as
        // a new-edge source: its row disappearing (or gaining a target) would
        // invalidate the stored-list re-validation / silently un-demonitor.
        // De-monitored values are exactly the boundary values whose stored
        // row is missing while the tree-derived full row is non-empty.
        //
        // New-edge sources must carry a runtime-live row: an emit-aliased
        // core.state.read result has a dead row (planReadAliases, cpu_emit.cpp)
        // and a core.dpi.call result is change-blind in the vchg profile
        // (unpriceable widen); both are rejected (NO00015 rules, mirrored).
        constexpr int64_t kMigrateEcSaveX4 = 16; // (K_DETECT + K_STORE) * 4
        constexpr int64_t kMigrateEcOpX4 = 13;   // K_OP * 4
        constexpr int64_t kMigrateEcSetX4 = 4;   // K_SET * 4

        // K_EVAL * 4 per producer kind (census mirror; unlisted kinds = 12).
        int64_t migrateEcEvalX4(const std::string_view name) {
            if (name == "core.compute.logicNot") return 4;
            if (name == "core.compute.and" || name == "core.compute.or" ||
                name == "core.compute.xor" || name == "core.compute.not" ||
                name == "core.compute.eq" || name == "core.compute.sliceStatic" ||
                name == "core.compute.bitSelect") return 8;
            return 12;
        }

        class MigrateBoundaryOpsEcPass final : public Pass {
        public:
            explicit MigrateBoundaryOpsEcPass(std::string profilePath)
                : Pass("grhsim.migrate-boundary-ops-ec", PassKind::BackendMapping),
                  profilePath_(std::move(profilePath)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                const auto *previous = model.cpuMapping();
                if (!previous || previous->stage != CpuMappingStage::Schedule ||
                    !previous->dataLayout || !previous->schedule)
                    throw std::runtime_error(
                        "grhsim.migrate-boundary-ops-ec requires a complete CPU schedule mapping");
                const auto &operations = model.operations();
                for (const auto &op : operations)
                    if (model.text(op.opType) == "core.compute.expr")
                        throw std::runtime_error(
                            "grhsim.migrate-boundary-ops-ec does not support models containing core.compute.expr");

                std::vector<uint64_t> writes(model.values().size() + 1, 0);
                std::vector<uint64_t> changes(model.values().size() + 1, 0);
                std::vector<uint64_t> body(previous->partitionTree.partitions.size() + 1, 0);
                loadProfile(writes, changes, body);

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

                // Stored fanout rows (post-demonitor state) and arm lists.
                std::vector<uint32_t> rowOf(valueCount + 1, 0);
                for (uint32_t i = 0; i < mapping.schedule->computeSupernodeFanout.size(); ++i)
                    rowOf[mapping.schedule->computeSupernodeFanout[i].source.index] = i + 1;
                const auto liveRow = [&](uint32_t value) {
                    const uint32_t row = rowOf[value];
                    return row && !mapping.schedule->computeSupernodeFanout[row - 1].targets.activate.empty();
                };

                // De-monitor closure: de-monitored values are exactly the
                // boundary values whose stored row is missing while the
                // tree-derived full row (compute-supernode consumers outside
                // the producer unit) is non-empty.
                std::vector<uint8_t> closure(valueCount + 1, 0);
                uint64_t demonitored = 0;
                for (uint32_t v = 1; v <= valueCount; ++v) {
                    if (layout.values[v - 1].kind != CpuStorageKind::Boundary || rowOf[v]) continue;
                    const uint32_t unitA = owner[producer[v]];
                    bool fullNonEmpty = false;
                    for (auto cons : consumers[v])
                        if (owner[cons] && owner[cons] != unitA) { fullNonEmpty = true; break; }
                    if (!fullNonEmpty) continue;
                    ++demonitored;
                    const uint32_t xopIdx = producer[v];
                    if (!xopIdx) continue;
                    for (auto operand : model.operands(operations[xopIdx - 1]))
                        if (!constantValue(operand.index)) closure[operand.index] = 1;
                }

                // Emit read-alias mirror (cpu_emit.cpp planReadAliases, same as
                // EdgeCompletionView in cpu_schedule.cpp): an aliased state
                // read result has a dead schedule row; DPI results are blind
                // in the vchg profile. Both are rejected as new-edge sources.
                std::vector<uint8_t> aliased(valueCount + 1, 0), dpiProduced(valueCount + 1, 0);
                {
                    std::vector<PartitionId> computeOwner(opCount + 1);
                    for (const auto &task : mapping.schedule->numaNodes[0].cores[0].tasks)
                        if (task.execution == CpuExecution::ActivityDrivenCompute)
                            for (const auto word : tree.partitions[task.partition.index - 1].children)
                                for (const auto unit : tree.partitions[word.index - 1].children)
                                    for (const auto node : tree.partitions[unit.index - 1].children)
                                        for (const auto op : tree.partitions[node.index - 1].ops)
                                            computeOwner[op.index] = unit;
                    std::vector<uint8_t> snapshot(valueCount + 1, 0);
                    for (const auto &op : operations)
                        if (!computeOwner[op.id.index])
                            for (auto operand : model.operands(op)) snapshot[operand.index] = 1;
                    for (const auto &partition : tree.partitions)
                        if (partition.attrs.eventGate)
                            for (const auto &event : partition.attrs.eventGate->events)
                                snapshot[event.value.index] = 1;
                    const auto &projection = mapping.schedule->quiescenceProjection;
                    for (const auto &op : operations) {
                        const auto name = model.text(op.opType);
                        if (name == "core.dpi.call") {
                            for (const auto result : model.results(op)) dpiProduced[result.index] = 1;
                            continue;
                        }
                        if (name != "core.state.read" || !computeOwner[op.id.index] ||
                            model.results(op).empty() || model.objectRefs(op).empty())
                            continue;
                        const auto result = model.results(op)[0];
                        const auto source = model.objectRefs(op)[0].index;
                        if (source < projection.size() && projection[source] &&
                            !snapshot[result.index] &&
                            model.types()[model.values()[result.index - 1].type.index - 1].kind ==
                                TypeKind::Logic &&
                            model.values()[result.index - 1].type == model.states()[source - 1].type)
                            aliased[result.index] = 1;
                    }
                }

                // Side-effect-free memo per unit (core.compute.* /
                // core.state.read / core.state.memRead only).
                std::vector<uint32_t> supernodeOf(tree.partitions.size() + 1, 0);
                for (uint32_t i = 0; i < supernodes.size(); ++i) supernodeOf[supernodes[i].part] = i + 1;
                std::vector<int8_t> safeMemo(tree.partitions.size() + 1, -1);
                const auto sideEffectFree = [&](uint32_t unit) {
                    auto &memo = safeMemo[unit];
                    if (memo >= 0) return memo != 0;
                    bool safe = true;
                    for (auto opIdx : supernodes[supernodeOf[unit] - 1].flat) {
                        const auto name = model.text(operations[opIdx - 1].opType);
                        if (name.size() >= 13 && name.compare(0, 13, "core.compute.") == 0) continue;
                        if (name == "core.state.read" || name == "core.state.memRead") continue;
                        safe = false;
                        break;
                    }
                    memo = safe ? int8_t(1) : int8_t(0);
                    return memo != 0;
                };

                struct Move { uint32_t op, value, from, to; };
                std::vector<Move> moves;
                std::vector<uint32_t> donorRemovals(tree.partitions.size() + 1, 0);
                uint64_t eligible = 0, profitable = 0, cascadeTrimmed = 0;
                int64_t saveX4 = 0, widenX4 = 0, reevalX4 = 0;
                // Candidate record for the fixpoint: per value, its op, target
                // and non-constant operands (edge sources included).
                struct Cand { uint32_t op, value, from, to; std::vector<uint32_t> operands;
                              int64_t widen, reeval; };
                std::vector<Cand> cands;
                std::vector<uint8_t> selected(valueCount + 1, 0);
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
                        if (closure[value]) continue;
                        const uint32_t rowIdx = rowOf[value];
                        if (!rowIdx) continue;
                        const auto &row = mapping.schedule->computeSupernodeFanout[rowIdx - 1];
                        if (row.targets.activate.empty() || !row.targets.arm.empty()) continue;
                        uint32_t target = 0;
                        bool ok = true;
                        for (auto consumer : consumers[value]) {
                            const uint32_t unit = owner[consumer];
                            if (unit == 0 || unit == sn.part || (target != 0 && unit != target)) {
                                ok = false;
                                break;
                            }
                            target = unit;
                        }
                        if (!ok || target == 0 || !sideEffectFree(target)) continue;
                        const auto &inputs = unitInputs[target];
                        const auto opsB = static_cast<int64_t>(
                            supernodes[supernodeOf[target] - 1].flat.size());
                        std::vector<uint32_t> operands;
                        int64_t widen = 0;
                        for (auto operand : model.operands(op)) {
                            const uint32_t w = operand.index;
                            if (owner[producer[w]] == target || inputs.count(w)) {
                                operands.push_back(w);
                                continue;
                            }
                            if (constantValue(w)) {
                                // Donor-local constant orphan rule (NO00013):
                                // migrating X would turn it into a new
                                // boundary value; constants never co-migrate.
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
                                        ok = false;
                                        break;
                                    }
                                }
                                continue;
                            }
                            // New edge source: w must carry a runtime-live row.
                            if (w == value || closure[w] || !producer[w] ||
                                layout.values[w - 1].kind != CpuStorageKind::Boundary ||
                                pinned[w] || !liveRow(w) || aliased[w] || dpiProduced[w]) {
                                ok = false;
                                break;
                            }
                            operands.push_back(w);
                            widen += static_cast<int64_t>(changes[w]) *
                                     ((opsB + 1) * kMigrateEcOpX4 + kMigrateEcSetX4);
                        }
                        if (!ok) continue;
                        ++eligible;
                        const int64_t reeval = static_cast<int64_t>(body[target]) * migrateEcEvalX4(name);
                        const int64_t profit =
                            static_cast<int64_t>(writes[value]) * kMigrateEcSaveX4 - widen - reeval;
                        if (profit <= 0) continue;
                        ++profitable;
                        selected[value] = 1;
                        cands.push_back({opIdx, value, sn.part, target, std::move(operands),
                                         widen, reeval});
                    }
                }

                // Greatest fixpoint over the profitable set: a migrated value's
                // non-constant operands must not themselves migrate (a migrated
                // operand loses its row / becomes unit-local).
                {
                    std::unordered_map<uint32_t, std::size_t> candOf;
                    for (std::size_t i = 0; i < cands.size(); ++i) candOf[cands[i].value] = i;
                    while (true) {
                        std::vector<uint32_t> drop;
                        for (const auto &cand : cands) {
                            if (!selected[cand.value]) continue;
                            for (const auto w : cand.operands)
                                if (selected[w]) {
                                    drop.push_back(cand.value);
                                    break;
                                }
                        }
                        if (drop.empty()) break;
                        for (const auto v : drop) {
                            selected[v] = 0;
                            ++cascadeTrimmed;
                        }
                    }
                    for (const auto &cand : cands)
                        if (selected[cand.value]) {
                            moves.push_back({cand.op, cand.value, cand.from, cand.to});
                            ++donorRemovals[cand.from];
                        }
                }

                // Donor keeps at least one op (active words pack eight aligned
                // consecutive supernodes); spare the last flat-order candidates
                // per emptied donor (NO00013 restore rule).
                uint64_t restored = 0;
                {
                    std::vector<uint32_t> flatSize(tree.partitions.size() + 1, 0);
                    for (const auto &sn : supernodes) flatSize[sn.part] = sn.flat.size();
                    std::vector<uint8_t> spared(tree.partitions.size() + 1, 0);
                    for (auto it = moves.end(); it != moves.begin();) {
                        --it;
                        if (donorRemovals[it->from] < flatSize[it->from] || spared[it->from]) continue;
                        spared[it->from] = 1;
                        --donorRemovals[it->from];
                        ++restored;
                        selected[it->value] = 0;
                        it->op = 0;  // mark dropped
                    }
                    moves.erase(std::remove_if(moves.begin(), moves.end(),
                                               [](const Move &m) { return m.op == 0; }),
                                moves.end());
                }
                // Aggregates over the final moved set (post-fixpoint,
                // post-restore) mirror the census selected aggregates.
                for (const auto &cand : cands) {
                    if (!selected[cand.value]) continue;
                    saveX4 += static_cast<int64_t>(writes[cand.value]) * kMigrateEcSaveX4;
                    widenX4 += cand.widen;
                    reevalX4 += cand.reeval;
                }
                if (moves.empty()) {
                    diagnostics.info("migrate_boundary_ops_ec=0 restored=" + std::to_string(restored) +
                        " eligible=" + std::to_string(eligible) +
                        " profitable=" + std::to_string(profitable) +
                        " cascade_trimmed=" + std::to_string(cascadeTrimmed) +
                        " demonitored=" + std::to_string(demonitored), name());
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
                    // consumer currently present (the cascade fixpoint keeps a
                    // migrated value's consumers from migrating out).
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
                                    "grhsim.migrate-boundary-ops-ec lost a consumer of a migrated value");
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
                            "grhsim.migrate-boundary-ops-ec emptied a compute supernode");
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
                                "grhsim.migrate-boundary-ops-ec helper chunk rebuild lost coverage");
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
                diagnostics.info("migrate_boundary_ops_ec=" + std::to_string(moves.size()) +
                    " donors=" + std::to_string(movesOut.size()) +
                    " targets=" + std::to_string(movesIn.size()) +
                    " dropped_nodes=" + std::to_string(droppedNodes) +
                    " restored=" + std::to_string(restored) +
                    " eligible=" + std::to_string(eligible) +
                    " profitable=" + std::to_string(profitable) +
                    " cascade_trimmed=" + std::to_string(cascadeTrimmed) +
                    " demonitored=" + std::to_string(demonitored) +
                    " save_x4=" + std::to_string(saveX4) +
                    " widen_x4=" + std::to_string(widenX4) +
                    " reeval_x4=" + std::to_string(reevalX4), name());
                return {true, true, {}};
            }

        private:
            void loadProfile(std::vector<uint64_t> &writes, std::vector<uint64_t> &changes,
                             std::vector<uint64_t> &body) const {
                std::ifstream input(profilePath_);
                if (!input)
                    throw std::runtime_error("grhsim.migrate-boundary-ops-ec cannot open profile: " + profilePath_);
                const std::string header = "# grhsim-migrate-ec-profile v1";
                std::string line;
                bool sawHeader = false;
                const auto parseUint = [](const char *&cursor, const char *end, uint64_t &out) {
                    while (cursor != end && *cursor == ' ') ++cursor;
                    const auto *begin = cursor;
                    while (cursor != end && *cursor != ' ') ++cursor;
                    if (begin == cursor) return false;
                    return std::from_chars(begin, cursor, out).ec == std::errc();
                };
                while (std::getline(input, line)) {
                    if (line.empty()) continue;
                    if (line.front() == '#') {
                        if (line.compare(0, header.size(), header) != 0)
                            throw std::runtime_error("grhsim.migrate-boundary-ops-ec bad profile header: " + line);
                        sawHeader = true;
                        continue;
                    }
                    const char *cursor = line.data();
                    const char *end = cursor + line.size();
                    uint64_t id = 0, a = 0, b = 0;
                    const char tag = *cursor;
                    if (tag != 'v' && tag != 'u')
                        throw std::runtime_error("grhsim.migrate-boundary-ops-ec bad profile row: " + line);
                    ++cursor;
                    if (!parseUint(cursor, end, id) || !parseUint(cursor, end, a))
                        throw std::runtime_error("grhsim.migrate-boundary-ops-ec bad profile row: " + line);
                    if (tag == 'v') {
                        if (!parseUint(cursor, end, b))
                            throw std::runtime_error("grhsim.migrate-boundary-ops-ec bad profile row: " + line);
                        if (!id || id >= writes.size())
                            throw std::runtime_error(
                                "grhsim.migrate-boundary-ops-ec profile value id out of range: " +
                                std::to_string(id) + " (stale profile?)");
                        writes[id] = a;
                        changes[id] = b;
                    } else {
                        if (!id || id >= body.size())
                            throw std::runtime_error(
                                "grhsim.migrate-boundary-ops-ec profile unit id out of range: " +
                                std::to_string(id) + " (stale profile?)");
                        body[id] = a;
                    }
                }
                if (!sawHeader)
                    throw std::runtime_error("grhsim.migrate-boundary-ops-ec profile missing header: " + profilePath_);
            }

            std::string profilePath_;
        };
    }

    void registerMigrateBoundaryOpsEcPass(PassRegistry &registry) {
        std::string error;
        if (!registry.registerPass("grhsim.migrate-boundary-ops-ec", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                std::string profile;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    if (args[i] == "--profile" && i + 1 < args.size()) {
                        profile = std::string(args[++i]);
                        continue;
                    }
                    factoryError = "grhsim.migrate-boundary-ops-ec expects --profile <path>";
                    return {};
                }
                if (profile.empty()) {
                    factoryError = "grhsim.migrate-boundary-ops-ec requires --profile <path>";
                    return {};
                }
                return std::make_unique<MigrateBoundaryOpsEcPass>(std::move(profile));
            }, error))
            throw std::logic_error(error);
    }
}
