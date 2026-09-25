#include "grhsim/pass/fuse_expr_chains.hpp"
#include "grhsim/backend/cpu.hpp"
#include "grhsim/ir/model.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <variant>
#include <vector>

namespace wolvrix::lib::grhsim {
    namespace {
        // Fuse single-use chains of pure scalar compute ops inside one compute
        // supernode helper chunk into one core.compute.expr op (NO00011). The tree
        // root keeps its OpId, result value and frame slot; intermediate ops stay
        // in the model and partition tables (mapping coverage is unchanged) and are
        // skipped by the CPU emitter, which rebuilds their per-op normalized
        // expressions inline from the postfix "tree" parameter. Runs after the CPU
        // schedule mapping so supernode/chunk membership is available; the
        // partition tree and schedule are preserved, while the data layout payload
        // (helper read caches, boundary densification) is recomputed since it is a
        // function of op operand structure.
        bool eligibleKind(std::string_view name) {
            static const std::array<std::string_view, 42> kinds{
                "core.compute.add", "core.compute.sub", "core.compute.mul",
                "core.compute.and", "core.compute.or", "core.compute.xor",
                "core.compute.xnor", "core.compute.not", "core.compute.shl",
                "core.compute.lshr", "core.compute.ashr", "core.compute.div",
                "core.compute.mod", "core.compute.eq", "core.compute.ne",
                "core.compute.caseEq", "core.compute.caseNe", "core.compute.wildcardEq",
                "core.compute.wildcardNe", "core.compute.lt", "core.compute.le",
                "core.compute.gt", "core.compute.ge", "core.compute.logicAnd",
                "core.compute.logicOr", "core.compute.logicNot", "core.compute.reduceAnd",
                "core.compute.reduceNand", "core.compute.reduceOr", "core.compute.reduceNor",
                "core.compute.reduceXor", "core.compute.reduceXnor", "core.compute.mux",
                "core.compute.bitSelect", "core.compute.prioritySelect", "core.compute.concat",
                "core.compute.replicate", "core.compute.sliceStatic", "core.compute.sliceDynamic",
                "core.compute.sliceArray", "core.compute.assign"};
            for (const auto kind : kinds)
                if (kind == name) return true;
            return false;
        }

        class FuseExprChainsPass final : public Pass {
        public:
            explicit FuseExprChainsPass(uint32_t maxTreeOps)
                : Pass("grhsim.fuse-expr-chains", PassKind::BackendMapping), maxTreeOps_(maxTreeOps) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                const auto *mapping = model.cpuMapping();
                if (!mapping || mapping->stage != CpuMappingStage::Schedule ||
                    !mapping->dataLayout || !mapping->schedule)
                    throw std::runtime_error("grhsim.fuse-expr-chains requires a complete CPU schedule mapping");
                const auto &operations = model.operations();
                const auto &layout = *mapping->dataLayout;
                const auto &schedule = *mapping->schedule;
                const std::size_t valueCount = model.values().size();

                std::vector<uint32_t> producer(valueCount + 1, 0), uses(valueCount + 1, 0),
                    soleConsumer(valueCount + 1, 0);
                for (const auto &op : operations)
                    for (auto result : model.results(op)) producer[result.index] = op.id.index;
                for (const auto &op : operations)
                    for (auto operand : model.operands(op)) ++uses[operand.index];
                for (const auto &op : operations)
                    for (auto operand : model.operands(op))
                        if (uses[operand.index] == 1) soleConsumer[operand.index] = op.id.index;

                // Chunk key per op: compute supernodes (supernode partitions with node
                // children) flatten their children's op lists; helperChunks partition
                // that order. Ops outside compute units get unique keys, so an edge
                // never matches across units, chunks or non-compute partitions.
                std::vector<uint32_t> chunkKey(operations.size() + 1, 0);
                std::vector<uint8_t> inComputeUnit(operations.size() + 1, 0);
                uint32_t nextKey = 1;
                for (const auto &partition : mapping->partitionTree.partitions) {
                    if (partition.attrs.kind != CpuPartitionKind::Supernode || partition.children.empty())
                        continue;
                    std::vector<OpId> ops;
                    bool nodesOnly = true;
                    for (auto node : partition.children) {
                        const auto &child = mapping->partitionTree.partitions[node.index - 1];
                        if (child.attrs.kind != CpuPartitionKind::Node) { nodesOnly = false; break; }
                        ops.insert(ops.end(), child.ops.begin(), child.ops.end());
                    }
                    if (!nodesOnly || ops.empty()) continue;
                    const auto &chunks = partition.attrs.helperChunks;
                    const uint32_t base = nextKey;
                    nextKey += std::max<std::size_t>(chunks.size(), 1);
                    std::size_t chunk = 0;
                    for (std::size_t pos = 0; pos < ops.size(); ++pos) {
                        if (!chunks.empty())
                            while (chunk + 1 < chunks.size() &&
                                   pos >= chunks[chunk].offset + chunks[chunk].count) ++chunk;
                        chunkKey[ops[pos].index] = base + static_cast<uint32_t>(chunk);
                        inComputeUnit[ops[pos].index] = 1;
                    }
                }
                for (const auto &op : operations)
                    if (!chunkKey[op.id.index]) chunkKey[op.id.index] = nextKey++;

                // Values whose frame slot must keep its per-op writer: boundary
                // slots, fanout/runtime/event-gate references.
                std::vector<uint8_t> protectedValue(valueCount + 1, 0);
                for (std::size_t i = 0; i < layout.values.size(); ++i)
                    if (layout.values[i].kind == CpuStorageKind::Boundary) protectedValue[i + 1] = 1;
                for (const auto &row : schedule.computeSupernodeFanout) protectedValue[row.source.index] = 1;
                for (const auto &row : schedule.inputFanout) protectedValue[row.source.index] = 1;
                for (const auto &slot : layout.runtime)
                    if (slot.value) protectedValue[slot.value.index] = 1;
                for (const auto &shadow : schedule.inputShadows) protectedValue[shadow.value.index] = 1;
                for (const auto &partition : mapping->partitionTree.partitions)
                    if (partition.attrs.eventGate)
                        for (const auto &event : partition.attrs.eventGate->events)
                            protectedValue[event.value.index] = 1;

                const auto scalarType = [&](ValueId value) {
                    const auto &type = model.types()[model.values()[value.index - 1].type.index - 1];
                    return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                           type.width >= 1 && type.width <= 64;
                };
                const auto eligible = [&](uint32_t position) {
                    const auto &op = operations[position];
                    if (!eligibleKind(model.text(op.opType))) return false;
                    const auto operands = model.operands(op), results = model.results(op);
                    if (results.size() != 1 || !model.objectRefs(op).empty()) return false;
                    for (const auto &param : model.parameters(op))
                        if (!std::holds_alternative<int64_t>(param.value)) return false;
                    if (!scalarType(results[0])) return false;
                    for (const auto value : operands)
                        if (!scalarType(value)) return false;
                    return true;
                };
                const auto fusableEdge = [&](uint32_t producerPos, uint32_t consumerPos) {
                    if (!eligible(producerPos) || !eligible(consumerPos)) return false;
                    const auto result = model.results(operations[producerPos])[0];
                    if (uses[result.index] != 1 ||
                        soleConsumer[result.index] != operations[consumerPos].id.index) return false;
                    if (protectedValue[result.index]) return false;
                    return chunkKey[operations[producerPos].id.index] ==
                           chunkKey[operations[consumerPos].id.index];
                };

                std::vector<uint8_t> claimed(operations.size(), 0);
                std::vector<std::string> tokens;
                std::vector<ValueId> leaves;
                std::unordered_map<uint32_t, uint32_t> leafIndex;
                std::vector<uint32_t> members;
                uint32_t treeCount = 0;
                const std::function<void(uint32_t)> visit = [&](uint32_t position) {
                    const auto &op = operations[position];
                    const auto operands = model.operands(op);
                    for (const auto operand : operands) {
                        const uint32_t source = producer[operand.index];
                        const uint32_t sourcePos = source - 1;
                        if (source && !claimed[sourcePos] && treeCount < maxTreeOps_ &&
                            fusableEdge(sourcePos, position)) {
                            claimed[sourcePos] = 1;
                            ++treeCount;
                            visit(sourcePos);
                        } else {
                            const auto [it, inserted] =
                                leafIndex.emplace(operand.index, static_cast<uint32_t>(leaves.size()));
                            if (inserted) leaves.push_back(operand);
                            tokens.push_back("l" + std::to_string(it->second));
                        }
                    }
                    const auto &resultType =
                        model.types()[model.values()[model.results(op)[0].index - 1].type.index - 1];
                    std::string token = "n;";
                    token += model.text(op.opType).substr(std::string_view("core.compute.").size());
                    token += ';' + std::to_string(resultType.width);
                    token += resultType.isSigned ? ";1" : ";0";
                    token += ';' + std::to_string(operands.size());
                    token += ';' + std::to_string(op.id.index);
                    for (const auto &param : model.parameters(op)) {
                        token += ';';
                        token += model.text(param.name);
                        token += '=';
                        token += std::to_string(std::get<int64_t>(param.value));
                    }
                    tokens.push_back(std::move(token));
                    members.push_back(position);
                };

                uint64_t trees = 0, nodes = 0, maxTree = 0;
                for (uint32_t position = 0; position < operations.size(); ++position) {
                    if (claimed[position] || !eligible(position) ||
                        !inComputeUnit[operations[position].id.index]) continue;
                    const auto result = model.results(operations[position])[0];
                    if (uses[result.index] == 0) continue;
                    // An op feeding its single consumer inside that consumer's tree is
                    // an intermediate, not a root — unless the consumer was already
                    // claimed (cap-truncated chain top), which starts a new segment.
                    if (uses[result.index] == 1 && soleConsumer[result.index]) {
                        const uint32_t consumer = soleConsumer[result.index] - 1;
                        if (!claimed[consumer] && fusableEdge(position, consumer)) continue;
                    }
                    tokens.clear(); leaves.clear(); leafIndex.clear(); members.clear();
                    treeCount = 1;
                    visit(position);
                    if (members.size() < 2) continue;
                    claimed[position] = 1;
                    const std::array<ValueId, 1> results{result};
                    const std::array<Parameter, 2> parameters{
                        Parameter{model.intern("tree"), tokens},
                        Parameter{model.intern("rk"), std::string(model.text(operations[position].opType))}};
                    model.replaceOperation(operations[position].id, "core.compute.expr",
                                           leaves, results, {}, parameters);
                    ++trees;
                    nodes += members.size();
                    maxTree = std::max<uint64_t>(maxTree, members.size());
                }
                if (trees != 0 && !refreshCpuDataLayout(model, diagnostics))
                    return {false, false, {}};
                diagnostics.info("fuse_expr_trees=" + std::to_string(trees) +
                    " fuse_expr_nodes=" + std::to_string(nodes) +
                    " fuse_expr_max_tree=" + std::to_string(maxTree), name());
                return {true, trees != 0, {}};
            }

        private:
            uint32_t maxTreeOps_ = 64;
        };
    }

    void registerFuseExprChainsPass(PassRegistry &registry) {
        std::string error;
        registry.registerPass("grhsim.fuse-expr-chains", PassKind::BackendMapping,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                uint32_t maxTreeOps = 64;
                if (args.size() == 2 && args[0] == "--max-tree-ops") {
                    try {
                        const auto parsed = std::stoul(std::string(args[1]));
                        if (parsed < 2 || parsed > 4096) throw std::out_of_range("max-tree-ops");
                        maxTreeOps = static_cast<uint32_t>(parsed);
                    } catch (const std::exception &) {
                        factoryError = "grhsim.fuse-expr-chains --max-tree-ops must be in [2, 4096]";
                        return {};
                    }
                } else if (!args.empty()) {
                    factoryError = "grhsim.fuse-expr-chains accepts only --max-tree-ops <n>";
                    return {};
                }
                return std::make_unique<FuseExprChainsPass>(maxTreeOps);
            }, error);
    }
}
