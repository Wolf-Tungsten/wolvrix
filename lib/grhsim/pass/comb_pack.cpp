#include "grhsim/pass/comb_pack.hpp"

#include "grhsim/pass/cone_extract.hpp"
#include "simplify_internal.hpp"

#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// grhsim.comb-pack (stage A4): pack isomorphic combinational lanes across the
// whole graph into wider pointwise logic, using declaration provenance, value
// names and array-shape structure to order lanes. Semantic port of the GRH
// comb-lane-pack algorithm to the GrhSIM core dialect. See
// docs/grhsim_ir/passes/comb-pack.md for the contract.

namespace wolvrix::lib::grhsim
{
    namespace
    {
        struct CombPackOptions
        {
            uint32_t minGroupSize = 4;
            uint32_t maxGroupSize = 16;
            uint32_t minPackedWidth = 32;
            uint32_t maxPackedWidth = 1024;
            uint32_t maxTreeNodes = 64;
            uint32_t maxRootGap = 128;
            bool enableMux = true;
            bool enableDeclaredRoots = true;
            bool enableOutputRoots = true;
            bool enableStorageDataRoots = true;
            std::filesystem::path report;
        };

        // Rejection counters reported as comb_pack_rejected_<reason>.
        struct Rejections
        {
            uint64_t width = 0;       // packed width outside [minPackedWidth, maxPackedWidth]
            uint64_t crossRoot = 0;   // a lane tree depends on another lane root of the group
            uint64_t provenance = 0;  // a lane carries non-migratable provenance slices
            uint64_t phase = 0;       // lane producers/consumers span multiple phases
            uint64_t build = 0;       // lane signatures drifted between collect and rewrite
        };

        struct TypeKey
        {
            uint32_t width = 0;
            bool isSigned = false;
            LogicDomain domain = LogicDomain::TwoState;
            friend bool operator==(const TypeKey &, const TypeKey &) = default;
        };

        std::string typeKeyText(TypeKey key)
        {
            return std::to_string(key.width) + "," + (key.isSigned ? "1" : "0") + "," +
                   std::to_string(static_cast<unsigned>(key.domain));
        }

        struct Analysis
        {
            bool valid = false;
            bool internal = false;
            uint32_t nodes = 0;
            std::string signature;
        };

        bool isInternalOpName(std::string_view name)
        {
            return name == "core.compute.not" || name == "core.compute.and" ||
                   name == "core.compute.or" || name == "core.compute.xor" ||
                   name == "core.compute.xnor" || name == "core.compute.assign" ||
                   name == "core.compute.mux";
        }

        class Analyzer
        {
        public:
            Analyzer(const GrhSimModel &model, const CombPackOptions &options)
                : m_(model), options_(options)
            {
                defs_.resize(m_.values().size() + 1);
                for (const auto &op : m_.operations())
                    for (const auto result : m_.results(op)) defs_[result.index] = op.id;
            }

            const SimOp *def(ValueId value) const
            {
                return value.index < defs_.size() && defs_[value.index].valid()
                           ? &m_.operations()[defs_[value.index].index - 1]
                           : nullptr;
            }

            std::span<const ValueId> operandsOf(const SimOp &op) const { return m_.operands(op); }

            TypeKey keyOf(TypeId id) const
            {
                const auto &type = m_.types()[id.index - 1];
                return {type.width, type.isSigned, type.domain};
            }

            void clearMemo() { memo_.clear(); }

            Analysis analyze(ValueId value)
            {
                if (!value.valid() || value.index >= defs_.size()) return {};
                if (const auto it = memo_.find(value.index); it != memo_.end()) return it->second;
                Analysis node;
                const auto finish = [&] {
                    memo_.emplace(value.index, node);
                    return node;
                };
                if (stack_.contains(value.index)) return {};
                const auto &simValue = m_.values()[value.index - 1];
                const TypeKey resultKey = keyOf(simValue.type);
                const SimOp *defOp = def(value);
                const auto leaf = [&]() -> Analysis {
                    // Only logic values may join a packed word; real/string/array
                    // values are not leaves and invalidate the whole tree.
                    if (m_.types()[simValue.type.index - 1].kind != TypeKind::Logic) return {};
                    node.valid = true;
                    node.internal = false;
                    node.nodes = 0;
                    node.signature = "leaf(" + typeKeyText(resultKey) + ")";
                    return finish();
                };
                if (!defOp || !isInternal(*defOp, resultKey)) return leaf();
                stack_.insert(value.index);
                std::vector<Analysis> operands;
                operands.reserve(m_.operands(*defOp).size());
                uint32_t nodes = 1;
                bool ok = true;
                for (const auto operand : m_.operands(*defOp))
                {
                    Analysis child = analyze(operand);
                    if (!child.valid) { ok = false; break; }
                    nodes += child.nodes;
                    if (nodes > options_.maxTreeNodes) { ok = false; break; }
                    operands.push_back(std::move(child));
                }
                stack_.erase(value.index);
                if (!ok) return finish();
                std::ostringstream signature;
                signature << m_.text(defOp->opType).substr(14) << "(" << typeKeyText(resultKey);
                appendParams(signature, *defOp);
                signature << ":";
                for (std::size_t i = 0; i < operands.size(); ++i)
                {
                    if (i) signature << "|";
                    signature << operands[i].signature;
                }
                signature << ")";
                node.valid = true;
                node.internal = true;
                node.nodes = nodes;
                node.signature = signature.str();
                return finish();
            }

        private:
            bool isInternal(const SimOp &op, TypeKey result) const
            {
                const std::string_view name = m_.text(op.opType);
                if (!isInternalOpName(name)) return false;
                const auto operands = m_.operands(op);
                const auto logic = [&](ValueId v) {
                    return m_.types()[m_.values()[v.index - 1].type.index - 1].kind == TypeKind::Logic;
                };
                const auto matches = [&](ValueId v) { return keyOf(m_.values()[v.index - 1].type) == result; };
                if (name == "core.compute.not" || name == "core.compute.assign")
                    return operands.size() == 1 && matches(operands[0]);
                if (name == "core.compute.and" || name == "core.compute.or" ||
                    name == "core.compute.xor" || name == "core.compute.xnor")
                    return operands.size() == 2 && matches(operands[0]) && matches(operands[1]);
                // mux: only under two-state semantics — a four-state select has
                // X-merge behaviour that the masked-select rewrite does not
                // reproduce, so the sel domain must match the (two-state) result.
                if (name == "core.compute.mux")
                {
                    if (!options_.enableMux || operands.size() != 3 || result.domain != LogicDomain::TwoState)
                        return false;
                    const TypeKey sel = keyOf(m_.values()[operands[0].index - 1].type);
                    return logic(operands[0]) && sel.width == 1 && sel.domain == result.domain &&
                           !sel.isSigned && matches(operands[1]) && matches(operands[2]);
                }
                return false;
            }

            void appendParams(std::ostringstream &out, const SimOp &op) const
            {
                std::vector<Parameter> params(m_.parameters(op).begin(), m_.parameters(op).end());
                std::sort(params.begin(), params.end(),
                          [&](const Parameter &a, const Parameter &b) { return m_.text(a.name) < m_.text(b.name); });
                out << "{";
                bool first = true;
                for (const auto &p : params)
                {
                    if (!first) out << ",";
                    first = false;
                    out << m_.text(p.name) << ":";
                    std::visit(
                        [&](const auto &value) {
                            using T = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<T, std::vector<bool>> ||
                                          std::is_same_v<T, std::vector<int64_t>> ||
                                          std::is_same_v<T, std::vector<double>> ||
                                          std::is_same_v<T, std::vector<std::string>>)
                            {
                                out << "[";
                                for (std::size_t i = 0; i < value.size(); ++i)
                                {
                                    if (i) out << ";";
                                    out << value[i];
                                }
                                out << "]";
                            }
                            else if constexpr (std::is_same_v<T, bool>)
                                out << (value ? 1 : 0);
                            else
                                out << value;
                        },
                        p.value);
                }
                out << "}";
            }

            const GrhSimModel &m_;
            const CombPackOptions &options_;
            std::vector<OpId> defs_;
            std::unordered_map<uint32_t, Analysis> memo_;
            std::unordered_set<uint32_t> stack_;
        };

        // Builds the packed tree for one lane group. All created ops inherit
        // the group phase; values inherit the lane type key at each position.
        class PackedBuilder
        {
        public:
            PackedBuilder(GrhSimModel &model, Analyzer &analyzer, const CombPackOptions &options)
                : m_(model), analyzer_(analyzer), options_(options)
            {
            }

            std::optional<ValueId> build(std::span<const ValueId> lanes, SimPhase phase)
            {
                if (lanes.empty()) return std::nullopt;
                analyzer_.clearMemo();
                const Analysis first = analyzer_.analyze(lanes.front());
                if (!first.valid) return std::nullopt;
                const auto &firstValue = m_.values()[lanes.front().index - 1];
                if (!first.internal) return concat(lanes, analyzer_.keyOf(firstValue.type), phase, {});
                std::vector<const SimOp *> defs;
                defs.reserve(lanes.size());
                for (const auto lane : lanes)
                {
                    const auto &laneValue = m_.values()[lane.index - 1];
                    if (analyzer_.keyOf(laneValue.type) != analyzer_.keyOf(firstValue.type)) return std::nullopt;
                    const Analysis laneAnalysis = analyzer_.analyze(lane);
                    if (!laneAnalysis.valid || laneAnalysis.signature != first.signature) return std::nullopt;
                    const SimOp *def = analyzer_.def(lane);
                    if (!def || def->phase != phase) return std::nullopt;
                    defs.push_back(def);
                }
                const auto &proto = *defs.front();
                const std::string_view name = m_.text(proto.opType);
                const auto operands = m_.operands(proto);
                // Copy out of the model pools: recursive builds append values
                // and ops, which may relocate the backing vectors.
                const OriginId protoOrigin = proto.origin;
                const TypeKey laneKey = analyzer_.keyOf(firstValue.type);
                const uint64_t packedWidth = uint64_t{laneKey.width} * lanes.size();
                if (packedWidth > std::numeric_limits<uint32_t>::max()) return std::nullopt;

                if (name == "core.compute.mux")
                {
                    std::vector<ValueId> conds, whenTrue, whenFalse;
                    for (const auto *def : defs)
                    {
                        const auto args = m_.operands(*def);
                        conds.push_back(args[0]);
                        whenTrue.push_back(args[1]);
                        whenFalse.push_back(args[2]);
                    }
                    auto packedTrue = build(whenTrue, phase);
                    auto packedFalse = build(whenFalse, phase);
                    if (!packedTrue || !packedFalse) return std::nullopt;
                    std::vector<ValueId> masks;
                    masks.reserve(conds.size());
                    for (const auto cond : conds)
                        masks.push_back(laneKey.width == 1 ? cond
                                                           : replicate(cond, laneKey.width, phase, protoOrigin));
                    const ValueId packedMask = concat(masks, laneKey, phase, protoOrigin);
                    const ValueId invMask = unary("core.compute.not", packedMask, packedWidth, laneKey, phase,
                                                  protoOrigin);
                    const ValueId trueMasked = binary("core.compute.and", *packedTrue, packedMask, packedWidth,
                                                      laneKey, phase, protoOrigin);
                    const ValueId falseMasked = binary("core.compute.and", *packedFalse, invMask, packedWidth,
                                                       laneKey, phase, protoOrigin);
                    return binary("core.compute.or", trueMasked, falseMasked, packedWidth, laneKey, phase,
                                  protoOrigin);
                }

                if (operands.size() == 1)
                {
                    std::vector<ValueId> childLanes;
                    childLanes.reserve(lanes.size());
                    for (const auto *def : defs) childLanes.push_back(m_.operands(*def)[0]);
                    auto packedChild = build(childLanes, phase);
                    if (!packedChild) return std::nullopt;
                    return unary(name, *packedChild, packedWidth, laneKey, phase, protoOrigin);
                }

                if (operands.size() == 2)
                {
                    std::vector<ValueId> lhsLanes, rhsLanes;
                    lhsLanes.reserve(lanes.size());
                    rhsLanes.reserve(lanes.size());
                    for (const auto *def : defs)
                    {
                        const auto args = m_.operands(*def);
                        lhsLanes.push_back(args[0]);
                        rhsLanes.push_back(args[1]);
                    }
                    auto packedLhs = build(lhsLanes, phase);
                    auto packedRhs = build(rhsLanes, phase);
                    if (!packedLhs || !packedRhs) return std::nullopt;
                    return binary(name, *packedLhs, *packedRhs, packedWidth, laneKey, phase, protoOrigin);
                }
                return std::nullopt;
            }

            uint64_t createdOps = 0;

        private:
            ValueId concat(std::span<const ValueId> lanes, TypeKey key, SimPhase phase, OriginId origin)
            {
                if (lanes.size() == 1) return lanes.front();
                uint64_t width = 0;
                for (const auto lane : lanes) width += analyzer_.keyOf(m_.values()[lane.index - 1].type).width;
                const auto out = m_.addValue(m_.logicType(static_cast<uint32_t>(width), key.isSigned, key.domain),
                                             {}, origin);
                std::vector<ValueId> operands(lanes.rbegin(), lanes.rend()); // lane 0 is the low bits
                const auto op = m_.addOperation("core.compute.concat", operands, std::array{out}, {}, {}, {}, origin);
                m_.setOperationPhase(op, phase);
                ++createdOps;
                return out;
            }

            ValueId unary(std::string_view name, ValueId operand, uint64_t width, TypeKey key, SimPhase phase,
                          OriginId origin)
            {
                const auto out = m_.addValue(m_.logicType(static_cast<uint32_t>(width), key.isSigned, key.domain),
                                             {}, origin);
                const auto op = m_.addOperation(name, std::array{operand}, std::array{out}, {}, {}, {}, origin);
                m_.setOperationPhase(op, phase);
                ++createdOps;
                return out;
            }

            ValueId binary(std::string_view name, ValueId lhs, ValueId rhs, uint64_t width, TypeKey key,
                           SimPhase phase, OriginId origin)
            {
                const auto out = m_.addValue(m_.logicType(static_cast<uint32_t>(width), key.isSigned, key.domain),
                                             {}, origin);
                const auto op = m_.addOperation(name, std::array{lhs, rhs}, std::array{out}, {}, {}, {}, origin);
                m_.setOperationPhase(op, phase);
                ++createdOps;
                return out;
            }

            ValueId replicate(ValueId operand, uint32_t rep, SimPhase phase, OriginId origin)
            {
                const auto key = analyzer_.keyOf(m_.values()[operand.index - 1].type);
                const auto out = m_.addValue(m_.logicType(key.width * rep, key.isSigned, key.domain), {}, origin);
                const std::array params{Parameter{m_.intern("rep"), static_cast<int64_t>(rep)}};
                const auto op = m_.addOperation("core.compute.replicate", std::array{operand}, std::array{out}, {},
                                                params, {}, origin);
                m_.setOperationPhase(op, phase);
                ++createdOps;
                return out;
            }

            GrhSimModel &m_;
            Analyzer &analyzer_;
            const CombPackOptions &options_;
        };

        struct RootCandidate
        {
            ValueId value;
            OpId rootOp;
            uint32_t anchorIndex = 0;
            std::string signature;
            std::string source;
            uint32_t laneWidth = 0;
            uint32_t treeNodes = 0;
            SimPhase phase = SimPhase::None;
            std::string family;            // declared-name pattern ('@' at index tokens); empty = no family
            std::vector<uint64_t> familyIndex; // numeric tokens, row-major
        };

        // Splits a declaration/value name into '_' tokens and abstracts every
        // all-numeric token into '@', returning the pattern plus the numeric
        // indices in row-major order. Pure ordering hint for lane adjacency.
        std::pair<std::string, std::vector<uint64_t>> familyOf(std::string_view name)
        {
            std::string pattern;
            std::vector<uint64_t> indices;
            bool numericAny = false;
            std::size_t begin = 0;
            const auto flush = [&](std::string_view token) {
                if (!pattern.empty()) pattern += '_';
                uint64_t number = 0;
                bool numeric = !token.empty() && token.size() <= 18;
                if (numeric)
                    for (const char c : token)
                    {
                        if (c < '0' || c > '9') { numeric = false; break; }
                        number = number * 10 + uint64_t(c - '0');
                    }
                if (numeric)
                {
                    pattern += '@';
                    indices.push_back(number);
                    numericAny = true;
                }
                else
                    pattern += token;
            };
            for (std::size_t i = 0; i <= name.size(); ++i)
                if (i == name.size() || name[i] == '_')
                {
                    flush(name.substr(begin, i - begin));
                    begin = i + 1;
                }
            if (!numericAny) return {};
            return {std::move(pattern), std::move(indices)};
        }

        bool dependsOnAnyOtherRoot(const Analyzer &analyzer, ValueId start,
                                   const std::unordered_set<uint32_t> &roots, ValueId self)
        {
            std::vector<ValueId> stack{start};
            std::unordered_set<uint32_t> visited;
            while (!stack.empty())
            {
                const ValueId current = stack.back();
                stack.pop_back();
                if (!current.valid() || !visited.insert(current.index).second) continue;
                if (current != self && roots.contains(current.index)) return true;
                const SimOp *def = analyzer.def(current);
                if (!def) continue;
                for (const auto operand : analyzer.operandsOf(*def)) stack.push_back(operand);
            }
            return false;
        }

        class CombPackPass final : public Pass
        {
        public:
            explicit CombPackPass(CombPackOptions options = {})
                : Pass("grhsim.comb-pack", PassKind::SemanticTransform), options_(std::move(options)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto originalOps = model.operations().size();
                const auto originalValues = model.values().size();
                Analyzer analyzer(model, options_);
                // Consumers of every value, as (op, operand slot) pairs; built
                // once and kept valid because rewrite only uses
                // replaceOperation (ids and slot order are stable).
                std::vector<std::vector<std::pair<OpId, uint32_t>>> uses(originalValues + 1);
                for (const auto &op : model.operations())
                {
                    const auto operands = model.operands(op);
                    for (uint32_t slot = 0; slot < operands.size(); ++slot)
                        uses[operands[slot].index].emplace_back(op.id, slot);
                }
                // Provenance migratability: a value may join a packed word only
                // when every slice targeting it is a Direct full-coverage slice
                // (the merge shifts targetOffset by the lane base).
                std::vector<uint8_t> provenanceExact(originalValues + 1, 1);
                std::vector<std::string_view> declaredName(originalValues + 1);
                for (const auto &record : model.declProvenances())
                {
                    for (const auto &slice : record.slices)
                    {
                        if (slice.target != DeclProvenanceTarget::Value || slice.targetIndex > originalValues)
                            continue;
                        const auto &value = model.values()[slice.targetIndex - 1];
                        const auto &type = model.types()[value.type.index - 1];
                        const bool exact = slice.kind == DeclProvenanceKind::Direct && slice.targetOffset == 0 &&
                                           slice.declOffset == 0 && slice.width == type.width;
                        if (!exact) provenanceExact[slice.targetIndex] = 0;
                        else if (declaredName[slice.targetIndex].empty())
                            declaredName[slice.targetIndex] = model.text(record.symbol);
                    }
                }

                std::unordered_map<std::string, std::vector<RootCandidate>> buckets;
                std::unordered_set<uint32_t> seenRoots;
                Rejections rejected;
                uint64_t candidates = 0;
                auto addCandidate = [&](ValueId value, OpId anchorOp, std::string_view source) {
                    if (!value.valid() || value.index > originalValues || !seenRoots.insert(value.index).second)
                        return;
                    const SimOp *def = analyzer.def(value);
                    if (!def) return;
                    const auto &type = model.types()[model.values()[value.index - 1].type.index - 1];
                    if (type.kind != TypeKind::Logic || type.width == 0) return;
                    const Analysis analysis = analyzer.analyze(value);
                    if (!analysis.valid || !analysis.internal) return;
                    const uint64_t minPacked = uint64_t{type.width} * options_.minGroupSize;
                    if (minPacked < options_.minPackedWidth || minPacked > options_.maxPackedWidth) return;
                    RootCandidate candidate;
                    candidate.value = value;
                    candidate.rootOp = def->id;
                    candidate.anchorIndex = anchorOp.index;
                    candidate.signature = analysis.signature;
                    candidate.source = std::string(source);
                    candidate.laneWidth = type.width;
                    candidate.treeNodes = analysis.nodes;
                    candidate.phase = def->phase;
                    if (!declaredName[value.index].empty())
                    {
                        auto [pattern, indices] = familyOf(declaredName[value.index]);
                        candidate.family = std::move(pattern);
                        candidate.familyIndex = std::move(indices);
                    }
                    buckets[candidate.signature].push_back(std::move(candidate));
                    ++candidates;
                };
                if (options_.enableDeclaredRoots)
                    for (const auto &record : model.declProvenances())
                        for (const auto &slice : record.slices)
                            if (slice.target == DeclProvenanceTarget::Value && slice.targetIndex >= 1 &&
                                slice.targetIndex <= originalValues)
                                if (const SimOp *def = analyzer.def(ValueId{slice.targetIndex}))
                                    addCandidate(ValueId{slice.targetIndex}, def->id, "declared");
                for (const auto &op : model.operations())
                {
                    const std::string_view name = model.text(op.opType);
                    const auto operands = model.operands(op);
                    if (options_.enableOutputRoots && name == "core.output.write" && operands.size() == 1)
                        addCandidate(operands[0], op.id, "output");
                    if (!options_.enableStorageDataRoots) continue;
                    const auto dataAt = [&](std::size_t index) {
                        if (index < operands.size()) addCandidate(operands[index], op.id, "storage-data");
                    };
                    if (name == "core.state.regWrite" || name == "core.state.latchWrite") dataAt(1);
                    else if (name == "core.state.memWrite") dataAt(2);
                    else if (name == "core.state.memFill" || name == "core.state.memAssign") dataAt(1);
                    else if (name == "core.state.memWriteSeq")
                    {
                        // Pre-lowering form carries len(event_edges) trailing
                        // event operands; the lowered form has no tail.
                        uint64_t events = 0;
                        for (const auto &p : model.parameters(op))
                            if (model.text(p.name) == "event_edges")
                                events = std::get<std::vector<std::string>>(p.value).size();
                        const std::size_t triples = (operands.size() - events) / 3;
                        for (std::size_t k = 0; k < triples; ++k) dataAt(2 + 3 * k);
                    }
                }

                struct Group
                {
                    std::vector<RootCandidate> roots;
                };
                std::vector<Group> groups;
                for (auto &[signature, bucket] : buckets)
                {
                    (void)signature;
                    std::sort(bucket.begin(), bucket.end(), [](const RootCandidate &a, const RootCandidate &b) {
                        if (a.family != b.family) return a.family < b.family;
                        if (a.familyIndex != b.familyIndex) return a.familyIndex < b.familyIndex;
                        return a.anchorIndex < b.anchorIndex;
                    });
                    std::size_t begin = 0;
                    while (begin < bucket.size())
                    {
                        std::size_t end = begin + 1;
                        while (end < bucket.size() &&
                               bucket[end].anchorIndex - bucket[end - 1].anchorIndex <= options_.maxRootGap)
                            ++end;
                        std::size_t cursor = begin;
                        while (cursor < end)
                        {
                            bool accepted = false;
                            uint64_t *firstFailure = nullptr;
                            std::size_t limit = std::min<std::size_t>(end - cursor, options_.maxGroupSize);
                            while (limit >= options_.minGroupSize)
                            {
                                std::vector<ValueId> values;
                                values.reserve(limit);
                                const SimPhase phase = bucket[cursor].phase;
                                bool eligible = true;
                                uint64_t *failure = nullptr;
                                uint64_t packedWidth = 0;
                                std::unordered_set<uint32_t> rootSet;
                                for (std::size_t i = 0; i < limit && eligible; ++i)
                                {
                                    const auto &candidate = bucket[cursor + i];
                                    values.push_back(candidate.value);
                                    rootSet.insert(candidate.value.index);
                                    packedWidth += candidate.laneWidth;
                                    if (!provenanceExact[candidate.value.index])
                                    {
                                        failure = &rejected.provenance;
                                        eligible = false;
                                    }
                                    else if (candidate.phase != phase)
                                    {
                                        failure = &rejected.phase;
                                        eligible = false;
                                    }
                                }
                                if (eligible &&
                                    (packedWidth < options_.minPackedWidth || packedWidth > options_.maxPackedWidth))
                                {
                                    failure = &rejected.width;
                                    eligible = false;
                                }
                                if (eligible)
                                {
                                    for (const auto value : values)
                                        if (dependsOnAnyOtherRoot(analyzer, value, rootSet, value))
                                        {
                                            failure = &rejected.crossRoot;
                                            eligible = false;
                                            break;
                                        }
                                }
                                if (eligible)
                                {
                                    Group group;
                                    for (std::size_t i = 0; i < limit; ++i) group.roots.push_back(bucket[cursor + i]);
                                    groups.push_back(std::move(group));
                                    cursor += limit;
                                    accepted = true;
                                    break;
                                }
                                if (!firstFailure) firstFailure = failure;
                                --limit;
                            }
                            if (!accepted)
                            {
                                if (firstFailure) ++*firstFailure;
                                ++cursor;
                            }
                        }
                        begin = end;
                    }
                }
                std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
                    return a.roots.front().anchorIndex < b.roots.front().anchorIndex;
                });

                PackedBuilder builder(model, analyzer, options_);
                std::vector<ValueId> mergeTarget(originalValues + 1);
                std::vector<uint64_t> mergeBase(originalValues + 1);
                std::vector<uint8_t> sweptCandidates(originalOps + 1, 1);
                uint64_t lanes = 0, packedGroups = 0;
                struct ReportRow
                {
                    uint32_t group, laneCount, laneWidth;
                    uint64_t packedWidth;
                    std::string source, family;
                };
                std::vector<ReportRow> reportRows;
                bool mutationStarted = false;
                try
                {
                    for (const auto &group : groups)
                    {
                        std::vector<ValueId> values;
                        values.reserve(group.roots.size());
                        for (const auto &candidate : group.roots) values.push_back(candidate.value);
                        const SimPhase phase = group.roots.front().phase;
                        // Consumers of every lane root must live in the group
                        // phase (or be unattributed) for the slice redirect to
                        // stay phase-legal.
                        bool phaseOk = true;
                        for (const auto value : values)
                            for (const auto &[consumer, slot] : uses[value.index])
                            {
                                (void)slot;
                                const auto consumerPhase = model.operations()[consumer.index - 1].phase;
                                if (consumerPhase != phase && consumerPhase != SimPhase::None)
                                {
                                    phaseOk = false;
                                    break;
                                }
                            }
                        if (!phaseOk)
                        {
                            ++rejected.phase;
                            continue;
                        }
                        auto packedRoot = builder.build(values, phase);
                        if (!packedRoot)
                        {
                            ++rejected.build;
                            continue;
                        }
                        mutationStarted = true;
                        const uint32_t laneWidth = group.roots.front().laneWidth;
                        for (std::size_t lane = 0; lane < values.size(); ++lane)
                        {
                            const ValueId oldValue = values[lane];
                            const auto &laneValue = model.values()[oldValue.index - 1];
                            const TypeKey laneKey = analyzer.keyOf(laneValue.type);
                            // Copy before addValue: appends may relocate the
                            // value pool.
                            const OriginId laneOrigin = laneValue.origin;
                            const uint64_t low = uint64_t{lane} * laneWidth;
                            const auto sliceValue = model.addValue(
                                model.logicType(laneWidth, laneKey.isSigned, laneKey.domain), {}, laneOrigin);
                            const std::array params{Parameter{model.intern("sliceStart"), static_cast<int64_t>(low)},
                                                    Parameter{model.intern("sliceEnd"),
                                                              static_cast<int64_t>(low + laneWidth - 1)}};
                            const auto sliceOp = model.addOperation("core.compute.sliceStatic",
                                                                    std::array{*packedRoot}, std::array{sliceValue},
                                                                    {}, params, {}, laneOrigin);
                            model.setOperationPhase(sliceOp, phase);
                            ++builder.createdOps;
                            for (const auto &[consumer, slot] : uses[oldValue.index])
                            {
                                const auto &op = model.operations()[consumer.index - 1];
                                const StringId opType = op.opType;
                                std::vector<ValueId> operands(model.operands(op).begin(), model.operands(op).end());
                                std::vector<ValueId> results(model.results(op).begin(), model.results(op).end());
                                std::vector<ObjectRef> refs(model.objectRefs(op).begin(), model.objectRefs(op).end());
                                std::vector<Parameter> params2(model.parameters(op).begin(), model.parameters(op).end());
                                operands[slot] = sliceValue;
                                model.replaceOperation(consumer, model.text(opType), operands, results, refs, params2);
                            }
                            mergeTarget[oldValue.index] = *packedRoot;
                            mergeBase[oldValue.index] = low;
                        }
                        lanes += values.size();
                        ++packedGroups;
                        if (!options_.report.empty())
                            reportRows.push_back({static_cast<uint32_t>(packedGroups),
                                                  static_cast<uint32_t>(values.size()), laneWidth,
                                                  uint64_t{laneWidth} * values.size(), group.roots.front().source,
                                                  group.roots.front().family});
                    }
                    if (packedGroups)
                    {
                        // Declarations of packed lanes re-target to their lane
                        // slice of the packed value (kind=Merged) before the
                        // sweep and compact drop the old cone values.
                        mergeProvenanceValueSlices(model, mergeTarget, mergeBase, DeclProvenanceKind::Merged);
                        // Keep cone ops whose result still carries a provenance
                        // slice (declared wires inside packed cones stay live
                        // anchors); everything else dead is swept.
                        for (const auto &record : model.declProvenances())
                            for (const auto &slice : record.slices)
                                if (slice.target == DeclProvenanceTarget::Value && slice.targetIndex >= 1 &&
                                    slice.targetIndex <= originalValues)
                                    if (const SimOp *def = analyzer.def(ValueId{slice.targetIndex}))
                                        sweptCandidates[def->id.index] = 0;
                        std::vector<OpId> candidates;
                        candidates.reserve(originalOps);
                        for (std::size_t i = 1; i <= originalOps; ++i)
                            if (sweptCandidates[i]) candidates.push_back(OpId{static_cast<uint32_t>(i)});
                        const auto dead = sweepDeadConeOps(model, candidates);
                        std::vector<uint8_t> removeOps(model.operations().size() + 1, 0);
                        for (const auto op : dead) removeOps[op.index] = 1;
                        const std::vector<uint8_t> removeStates(model.states().size() + 1, 0);
                        model.compact(removeOps, removeStates);
                    }
                }
                catch (const std::exception &error)
                {
                    if (mutationStarted) model.poison();
                    diagnostics.error(error.what(), name());
                    return {false, mutationStarted, {}};
                }
                if (!options_.report.empty())
                {
                    std::ofstream out(options_.report);
                    if (!out)
                    {
                        diagnostics.error("cannot open comb-pack report", name());
                        return {false, packedGroups != 0, {}};
                    }
                    out << "group\tlanes\tlane_width\tpacked_width\tsource\tfamily\n";
                    for (const auto &row : reportRows)
                        out << row.group << '\t' << row.laneCount << '\t' << row.laneWidth << '\t' << row.packedWidth
                            << '\t' << row.source << '\t' << (row.family.empty() ? "-" : row.family) << '\n';
                    if (!out)
                    {
                        diagnostics.error("cannot write comb-pack report", name());
                        return {false, packedGroups != 0, {}};
                    }
                }
                diagnostics.info(
                    "comb_pack_candidates=" + std::to_string(candidates) +
                    " comb_pack_groups=" + std::to_string(packedGroups) +
                    " comb_pack_lanes=" + std::to_string(lanes) +
                    " comb_pack_created_ops=" + std::to_string(builder.createdOps) +
                    " comb_pack_rejected_width=" + std::to_string(rejected.width) +
                    " comb_pack_rejected_cross_root=" + std::to_string(rejected.crossRoot) +
                    " comb_pack_rejected_provenance=" + std::to_string(rejected.provenance) +
                    " comb_pack_rejected_phase=" + std::to_string(rejected.phase) +
                    " comb_pack_rejected_build=" + std::to_string(rejected.build), name());
                return {true, packedGroups != 0, {}};
            }

        private:
            CombPackOptions options_;
        };

        bool parseUint(std::string_view text, uint32_t &out)
        {
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
            return ec == std::errc{} && end == text.data() + text.size();
        }

        std::unique_ptr<Pass> createCombPackPass(std::span<const std::string_view> args, std::string &error)
        {
            CombPackOptions options;
            for (std::size_t i = 0; i < args.size(); i += 2)
            {
                if (i + 1 == args.size())
                {
                    error = "grhsim.comb-pack option requires a value";
                    return {};
                }
                const auto name = args[i], value = args[i + 1];
                if (name == "--report") options.report = std::filesystem::path(value);
                else if (name == "--min-group-size")
                {
                    if (!parseUint(value, options.minGroupSize) || options.minGroupSize < 2)
                    {
                        error = "invalid min-group-size (must be >= 2)";
                        return {};
                    }
                }
                else if (name == "--max-group-size")
                {
                    if (!parseUint(value, options.maxGroupSize))
                    {
                        error = "invalid max-group-size";
                        return {};
                    }
                }
                else if (name == "--min-packed-width")
                {
                    if (!parseUint(value, options.minPackedWidth))
                    {
                        error = "invalid min-packed-width";
                        return {};
                    }
                }
                else if (name == "--max-packed-width")
                {
                    if (!parseUint(value, options.maxPackedWidth))
                    {
                        error = "invalid max-packed-width";
                        return {};
                    }
                }
                else if (name == "--max-tree-nodes")
                {
                    if (!parseUint(value, options.maxTreeNodes) || options.maxTreeNodes == 0)
                    {
                        error = "invalid max-tree-nodes";
                        return {};
                    }
                }
                else if (name == "--max-root-gap")
                {
                    if (!parseUint(value, options.maxRootGap))
                    {
                        error = "invalid max-root-gap";
                        return {};
                    }
                }
                else
                {
                    bool *flag = nullptr;
                    if (name == "--enable-mux") flag = &options.enableMux;
                    if (name == "--enable-declared-roots") flag = &options.enableDeclaredRoots;
                    if (name == "--enable-output-roots") flag = &options.enableOutputRoots;
                    if (name == "--enable-storage-data-roots") flag = &options.enableStorageDataRoots;
                    if (!flag || (value != "true" && value != "false"))
                    {
                        error = "unknown grhsim.comb-pack option or invalid boolean: " + std::string(name);
                        return {};
                    }
                    *flag = value == "true";
                }
            }
            if (options.maxGroupSize < options.minGroupSize)
            {
                error = "max-group-size must be >= min-group-size";
                return {};
            }
            if (options.maxPackedWidth < options.minPackedWidth)
            {
                error = "max-packed-width must be >= min-packed-width";
                return {};
            }
            if (!options.enableDeclaredRoots && !options.enableOutputRoots && !options.enableStorageDataRoots)
            {
                error = "at least one root source must stay enabled";
                return {};
            }
            return std::make_unique<CombPackPass>(std::move(options));
        }
    } // namespace

    void registerCombPackPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass("grhsim.comb-pack", PassKind::SemanticTransform, createCombPackPass, error);
    }
} // namespace wolvrix::lib::grhsim
