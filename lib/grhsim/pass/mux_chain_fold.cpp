#include "grhsim/pass/mux_chain_fold.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <vector>

namespace wolvrix::lib::grhsim {
    namespace {
        // Fold a priority mux chain  mux(c0, a0, mux(c1, a1, ... mux(cN-1, aN-1, d)))
        // into one core.compute.prioritySelect op with operands
        // [c0..cN-1, a0..aN-1, d]: the first true condition wins, exactly the chain
        // semantics, so no mutual-exclusion proof is needed. A link must be a plain
        // two-state scalar mux whose result has a single use — the false arm of the
        // parent link — and share the parent's result type, so per-link
        // normalization is idempotent and eliminating the intermediate values is
        // unobservable. Long chains are segmented to the 64-bit condition mask.
        class MuxChainFoldPass final : public Pass {
        public:
            MuxChainFoldPass() : Pass("grhsim.mux-chain-fold", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override {
                constexpr std::size_t minLinks = 3, maxLinks = 64;
                const auto &operations = model.operations();
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

                const auto eligible = [&](uint32_t index) {
                    const auto &op = operations[index];
                    if (model.text(op.opType) != "core.compute.mux") return false;
                    const auto operands = model.operands(op), results = model.results(op);
                    if (operands.size() != 3 || results.size() != 1 ||
                        !model.objectRefs(op).empty() || !model.parameters(op).empty()) return false;
                    const auto &resultType = model.types()[model.values()[results[0].index - 1].type.index - 1];
                    if (resultType.kind != TypeKind::Logic || resultType.domain != LogicDomain::TwoState ||
                        resultType.width == 0 || resultType.width > 64) return false;
                    const auto &condType = model.types()[model.values()[operands[0].index - 1].type.index - 1];
                    return condType.kind == TypeKind::Logic && condType.domain == LogicDomain::TwoState &&
                        condType.width == 1 && !condType.isSigned;
                };

                std::vector<uint8_t> claimed(operations.size() + 1, 0), removeOps(operations.size() + 1, 0);
                uint64_t folds = 0, segments = 0, links = 0, maxChain = 0;
                for (uint32_t index = 0; index < operations.size(); ++index) {
                    if (claimed[index] || !eligible(index)) continue;
                    // A chain root is not the single-use false-arm input of an
                    // eligible mux with the same result type.
                    const auto head = model.results(operations[index])[0];
                    bool root = true;
                    if (uses[head.index] == 1 && soleConsumer[head.index]) {
                        const uint32_t parent = soleConsumer[head.index] - 1;
                        if (eligible(parent) &&
                            model.operands(operations[parent])[2] == head &&
                            model.values()[model.results(operations[parent])[0].index - 1].type ==
                            model.values()[head.index - 1].type)
                            root = false;
                    }
                    if (!root) continue;
                    std::vector<uint32_t> chain{index};
                    while (chain.size() < 4096) {
                        const auto operands = model.operands(operations[chain.back()]);
                        const ValueId falseArm = operands[2];
                        const uint32_t next = producer[falseArm.index];
                        if (!next || claimed[next - 1] || !eligible(next - 1)) break;
                        if (uses[falseArm.index] != 1 ||
                            soleConsumer[falseArm.index] != operations[chain.back()].id.index) break;
                        if (model.values()[model.results(operations[next - 1])[0].index - 1].type !=
                            model.values()[model.results(operations[index])[0].index - 1].type) break;
                        chain.push_back(next - 1);
                    }
                    if (chain.size() < minLinks) continue;
                    maxChain = std::max<uint64_t>(maxChain, chain.size());
                    // Fold from the top in segments of at most maxLinks; a short
                    // tail stays plain muxes and becomes the segment's default arm.
                    std::size_t offset = 0;
                    while (chain.size() - offset >= minLinks) {
                        const std::size_t count = std::min(maxLinks, chain.size() - offset);
                        std::vector<ValueId> inputs;
                        inputs.reserve(2 * count + 1);
                        for (std::size_t i = 0; i < count; ++i)
                            inputs.push_back(model.operands(operations[chain[offset + i]])[0]);
                        for (std::size_t i = 0; i < count; ++i)
                            inputs.push_back(model.operands(operations[chain[offset + i]])[1]);
                        inputs.push_back(offset + count < chain.size()
                            ? model.results(operations[chain[offset + count]])[0]
                            : model.operands(operations[chain[offset + count - 1]])[2]);
                        const std::array<ValueId, 1> outputs{model.results(operations[chain[offset]])[0]};
                        model.replaceOperation(operations[chain[offset]].id, "core.compute.prioritySelect",
                                               inputs, outputs);
                        for (std::size_t i = 1; i < count; ++i) removeOps[operations[chain[offset + i]].id.index] = 1;
                        for (std::size_t i = 0; i < count; ++i) claimed[chain[offset + i]] = 1;
                        ++segments;
                        links += count;
                        offset += count;
                    }
                    ++folds;
                }
                if (folds) model.compact(removeOps, std::vector<uint8_t>(model.states().size() + 1));
                diagnostics.info("mux_chain_folds=" + std::to_string(folds) +
                    " mux_chain_segments=" + std::to_string(segments) +
                    " mux_chain_links=" + std::to_string(links) +
                    " mux_chain_max_links=" + std::to_string(maxChain), name());
                return {true, folds != 0, {}};
            }
        };
    }

    void registerMuxChainFoldPass(PassRegistry &registry) {
        std::string error;
        registry.registerPass("grhsim.mux-chain-fold", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                if (!args.empty()) {
                    factoryError = "grhsim.mux-chain-fold does not accept arguments";
                    return {};
                }
                return std::make_unique<MuxChainFoldPass>();
            }, error);
    }
}
