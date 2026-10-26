#include "transform/comb_loop_elim.hpp"

#include "core/grh.hpp"
#include "core/transform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::Graph;
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::SrcLoc;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;

        // CSR successor graph over live values, keyed by ValueId.index slots
        // (1-based within one graph; identical per-value edge order to the
        // append order used when building it).
        struct DenseSucc
        {
            std::vector<uint64_t> offsets;
            std::vector<ValueId> targets;

            std::size_t maxSlot() const noexcept
            {
                return offsets.size() >= 2 ? offsets.size() - 2 : 0;
            }

            std::span<const ValueId> edges(ValueId v) const noexcept
            {
                const uint64_t begin = offsets[v.index];
                const uint64_t end = offsets[v.index + 1];
                return std::span<const ValueId>(targets.data() + begin,
                                                static_cast<std::size_t>(end - begin));
            }
        };

        struct LoopInfo
        {
            std::vector<ValueId> loopValues;
            std::vector<OperationId> loopOps;
            bool isFalseLoop = false;
            bool wasFalseLoopCandidate = false;
            bool analysisIncomplete = false;
            bool hasDynamicArrayRead = false;
        };

        struct Scc
        {
            std::vector<ValueId> nodes;
        };

        struct TarjanState
        {
            const DenseSucc &succ;
            std::vector<int32_t> index;
            std::vector<int32_t> lowlink;
            std::vector<ValueId> stack;
            std::vector<char> onStack;
            std::vector<Scc> sccs;
            int nextIndex = 1;
        };

        struct BitRange
        {
            int64_t low = 0;
            int64_t high = -1;
        };

        struct RangeNode
        {
            ValueId value;
            BitRange range;
        };

        inline bool operator==(const RangeNode &lhs, const RangeNode &rhs)
        {
            return lhs.value == rhs.value &&
                   lhs.range.low == rhs.range.low &&
                   lhs.range.high == rhs.range.high;
        }

        struct RangeNodeHash
        {
            std::size_t operator()(const RangeNode &node) const noexcept
            {
                std::size_t seed = ValueIdHash{}(node.value);
                seed = seed * 1315423911u + std::hash<long long>{}(node.range.low);
                seed = seed * 1315423911u + std::hash<long long>{}(node.range.high);
                return seed;
            }
        };

        using RangeSuccMap = std::unordered_map<RangeNode, std::vector<RangeNode>, RangeNodeHash>;
        using RangeNodeSet = std::unordered_set<RangeNode, RangeNodeHash>;

        struct RangeScc
        {
            std::vector<RangeNode> nodes;
        };

        struct RangeTarjanState
        {
            const RangeSuccMap &succ;
            std::unordered_map<RangeNode, int, RangeNodeHash> index;
            std::unordered_map<RangeNode, int, RangeNodeHash> lowlink;
            std::vector<RangeNode> stack;
            std::unordered_set<RangeNode, RangeNodeHash> onStack;
            std::vector<RangeScc> sccs;
            int nextIndex = 0;
        };

        bool isBoundaryOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kConstant:
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterReadArrayPort:
            case OperationKind::kLatchReadPort:
            case OperationKind::kDpicCall:
                return true;
            default:
                return false;
            }
        }

        // stage-07: ops without a bit-level copy map are rewritten
        // monolithically when splitting a false loop — the result stays one
        // whole fragment (no cut points) and operands are reassembled as full
        // views. kRegisterReadArrayPort joins here (not via boundary skip): a
        // state read cannot be fragmented, but its index operand is ordinary
        // combinational input that must keep its (reassembled) cone.
        bool isMonolithicLoopOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kSub:
            case OperationKind::kMul:
            case OperationKind::kDiv:
            case OperationKind::kMod:
            case OperationKind::kShl:
            case OperationKind::kLShr:
            case OperationKind::kAShr:
            case OperationKind::kRegisterReadArrayPort:
                return true;
            default:
                return false;
            }
        }

        void tarjanVisit(ValueId v, TarjanState &state)
        {
            const std::size_t slot = v.index;
            state.index[slot] = state.nextIndex;
            state.lowlink[slot] = state.nextIndex;
            ++state.nextIndex;
            state.stack.push_back(v);
            state.onStack[slot] = 1;

            for (ValueId w : state.succ.edges(v))
            {
                const std::size_t wSlot = w.index;
                if (state.index[wSlot] == 0)
                {
                    tarjanVisit(w, state);
                    state.lowlink[slot] = std::min(state.lowlink[slot], state.lowlink[wSlot]);
                }
                else if (state.onStack[wSlot])
                {
                    state.lowlink[slot] = std::min(state.lowlink[slot], state.index[wSlot]);
                }
            }

            if (state.lowlink[slot] == state.index[slot])
            {
                Scc scc;
                while (!state.stack.empty())
                {
                    ValueId w = state.stack.back();
                    state.stack.pop_back();
                    state.onStack[w.index] = 0;
                    scc.nodes.push_back(w);
                    if (w == v)
                    {
                        break;
                    }
                }
                state.sccs.push_back(std::move(scc));
            }
        }

        std::vector<Scc> tarjanScc(std::span<const ValueId> nodes, const DenseSucc &succ)
        {
            TarjanState state{succ};
            const std::size_t slotCount = succ.maxSlot() + 1;
            state.index.assign(slotCount, 0);
            state.lowlink.assign(slotCount, 0);
            state.onStack.assign(slotCount, 0);
            state.stack.reserve(nodes.size());
            for (ValueId v : nodes)
            {
                if (state.index[v.index] == 0)
                {
                    tarjanVisit(v, state);
                }
            }
            return state.sccs;
        }

        bool hasSelfLoop(ValueId v, const DenseSucc &succ)
        {
            const auto edges = succ.edges(v);
            return std::find(edges.begin(), edges.end(), v) != edges.end();
        }

        void rangeTarjanVisit(const RangeNode &v, RangeTarjanState &state)
        {
            state.index.emplace(v, state.nextIndex);
            state.lowlink.emplace(v, state.nextIndex);
            ++state.nextIndex;
            state.stack.push_back(v);
            state.onStack.insert(v);

            auto it = state.succ.find(v);
            if (it != state.succ.end())
            {
                for (const RangeNode &w : it->second)
                {
                    if (state.index.find(w) == state.index.end())
                    {
                        rangeTarjanVisit(w, state);
                        state.lowlink[v] = std::min(state.lowlink[v], state.lowlink[w]);
                    }
                    else if (state.onStack.find(w) != state.onStack.end())
                    {
                        state.lowlink[v] = std::min(state.lowlink[v], state.index[w]);
                    }
                }
            }

            if (state.lowlink[v] == state.index[v])
            {
                RangeScc scc;
                while (!state.stack.empty())
                {
                    RangeNode w = state.stack.back();
                    state.stack.pop_back();
                    state.onStack.erase(w);
                    scc.nodes.push_back(w);
                    if (w == v)
                    {
                        break;
                    }
                }
                state.sccs.push_back(std::move(scc));
            }
        }

        std::vector<RangeScc> tarjanScc(const std::vector<RangeNode> &nodes, const RangeSuccMap &succ)
        {
            RangeTarjanState state{succ};
            state.index.reserve(nodes.size());
            state.lowlink.reserve(nodes.size());
            state.stack.reserve(nodes.size());
            for (const RangeNode &v : nodes)
            {
                if (state.index.find(v) == state.index.end())
                {
                    rangeTarjanVisit(v, state);
                }
            }
            return state.sccs;
        }

        bool hasSelfLoop(const RangeNode &v, const RangeSuccMap &succ)
        {
            auto it = succ.find(v);
            if (it == succ.end())
            {
                return false;
            }
            const auto &edges = it->second;
            return std::find(edges.begin(), edges.end(), v) != edges.end();
        }

        std::optional<int64_t> getIntAttr(const Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<int64_t>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::optional<int64_t> getIntAttr(const Graph &graph, OperationId opId, std::string_view key)
        {
            for (const auto &attr : graph.opAttrs(opId))
            {
                if (attr.key != key)
                {
                    continue;
                }
                if (const auto *value = std::get_if<int64_t>(&attr.value))
                {
                    return *value;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        std::optional<int64_t> parseConstIntLiteral(std::string_view literal)
        {
            if (literal.empty())
            {
                return std::nullopt;
            }
            if (literal.front() == '"' || literal.front() == '$')
            {
                return std::nullopt;
            }
            std::string cleaned;
            cleaned.reserve(literal.size());
            for (char ch : literal)
            {
                if (ch == '_' || std::isspace(static_cast<unsigned char>(ch)))
                {
                    continue;
                }
                cleaned.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (cleaned.empty())
            {
                return std::nullopt;
            }
            int base = 10;
            std::string digits;
            const std::size_t tick = cleaned.find('\'');
            if (tick != std::string::npos)
            {
                if (tick + 1 >= cleaned.size())
                {
                    return std::nullopt;
                }
                const char baseChar = cleaned[tick + 1];
                switch (baseChar)
                {
                case 'b':
                    base = 2;
                    break;
                case 'o':
                    base = 8;
                    break;
                case 'd':
                    base = 10;
                    break;
                case 'h':
                    base = 16;
                    break;
                default:
                    return std::nullopt;
                }
                if (tick + 2 >= cleaned.size())
                {
                    return std::nullopt;
                }
                digits = cleaned.substr(tick + 2);
            }
            else
            {
                digits = cleaned;
                if (digits.rfind("0x", 0) == 0)
                {
                    base = 16;
                    digits = digits.substr(2);
                }
                else if (digits.rfind("0b", 0) == 0)
                {
                    base = 2;
                    digits = digits.substr(2);
                }
                else if (digits.rfind("0o", 0) == 0)
                {
                    base = 8;
                    digits = digits.substr(2);
                }
            }
            if (digits.empty())
            {
                return std::nullopt;
            }
            for (char ch : digits)
            {
                if (ch == 'x' || ch == 'z' || ch == '?')
                {
                    return std::nullopt;
                }
            }
            try
            {
                return std::stoll(digits, nullptr, base);
            }
            catch (const std::exception &)
            {
                return std::nullopt;
            }
        }

        std::optional<uint64_t> getConstUintValue(const Graph &graph, ValueId valueId, int depth)
        {
            if (depth <= 0)
            {
                return std::nullopt;
            }
            if (!valueId.valid())
            {
                return std::nullopt;
            }
            const OperationId defId = graph.valueDef(valueId);
            if (!defId.valid())
            {
                return std::nullopt;
            }
            switch (graph.opKind(defId))
            {
            case OperationKind::kConstant:
            {
                for (const auto &attr : graph.opAttrs(defId))
                {
                    if (attr.key != "constValue")
                    {
                        continue;
                    }
                    if (const auto *literal = std::get_if<std::string>(&attr.value))
                    {
                        auto parsed = parseConstIntLiteral(*literal);
                        if (!parsed || *parsed < 0)
                        {
                            return std::nullopt;
                        }
                        return static_cast<uint64_t>(*parsed);
                    }
                    return std::nullopt;
                }
                return std::nullopt;
            }
            case OperationKind::kAssign:
            {
                const auto operands = graph.opOperands(defId);
                if (operands.size() != 1 || !operands[0].valid())
                {
                    return std::nullopt;
                }
                return getConstUintValue(graph, operands[0], depth - 1);
            }
            case OperationKind::kSliceStatic:
            {
                const auto operands = graph.opOperands(defId);
                if (operands.size() != 1 || !operands[0].valid())
                {
                    return std::nullopt;
                }
                auto startOpt = getIntAttr(graph, defId, "sliceStart");
                auto endOpt = getIntAttr(graph, defId, "sliceEnd");
                if (!startOpt || !endOpt)
                {
                    return std::nullopt;
                }
                const int64_t low = std::min(*startOpt, *endOpt);
                const int64_t high = std::max(*startOpt, *endOpt);
                const int64_t width = high - low + 1;
                if (low < 0 || width <= 0 || width > 63)
                {
                    return std::nullopt;
                }
                auto baseValue = getConstUintValue(graph, operands[0], depth - 1);
                if (!baseValue)
                {
                    return std::nullopt;
                }
                const uint64_t shifted = (*baseValue >> static_cast<uint64_t>(low));
                const uint64_t mask = (uint64_t(1) << static_cast<uint64_t>(width)) - 1u;
                return shifted & mask;
            }
            case OperationKind::kConcat:
            case OperationKind::kArrayCreate:
            {
                const auto operands = graph.opOperands(defId);
                if (operands.empty())
                {
                    return std::nullopt;
                }
                uint64_t acc = 0;
                int totalWidth = 0;
                for (std::size_t i = 0; i < operands.size(); ++i)
                {
                    const ValueId operand = operands[graph.opKind(defId) == OperationKind::kArrayCreate
                        ? operands.size() - 1 - i : i];
                    if (!operand.valid())
                    {
                        return std::nullopt;
                    }
                    const int64_t width = graph.valueWidth(operand);
                    if (width <= 0 || width > 63 || totalWidth + static_cast<int>(width) > 63)
                    {
                        return std::nullopt;
                    }
                    auto part = getConstUintValue(graph, operand, depth - 1);
                    if (!part)
                    {
                        return std::nullopt;
                    }
                    const uint64_t mask = (uint64_t(1) << static_cast<uint64_t>(width)) - 1u;
                    acc = (acc << static_cast<uint64_t>(width)) | (*part & mask);
                    totalWidth += static_cast<int>(width);
                }
                return acc;
            }
            default:
                return std::nullopt;
            }
        }

        std::optional<int64_t> getConstIntValue(const Graph &graph, ValueId valueId)
        {
            auto value = getConstUintValue(graph, valueId, 32);
            if (!value)
            {
                return std::nullopt;
            }
            if (*value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                return std::nullopt;
            }
            return static_cast<int64_t>(*value);
        }

        // Geometry of a kSliceArray op (stage-02 semantics: operands
        // [array, i0..ik-1], no attrs; row-major flat layout over the base
        // shape). `selectable` is true when every index resolved to an
        // in-bounds constant, in which case `offset`/`subVolume` give the
        // exact selected bit range of the base. Otherwise the read is treated
        // as dynamic: it may touch the whole array.
        struct SliceArrayGeometry
        {
            int64_t elementWidth = 0;
            int64_t subVolume = 0;
            std::size_t indexCount = 0;
            bool selectable = false;
            int64_t offset = 0;
        };

        std::optional<SliceArrayGeometry> getSliceArrayGeometry(const Graph &graph, const Operation &op)
        {
            if (op.kind() != OperationKind::kSliceArray)
            {
                return std::nullopt;
            }
            const auto &operands = op.operands();
            const auto &results = op.results();
            if (operands.size() < 2 || results.size() != 1 ||
                !operands[0].valid() || !results[0].valid())
            {
                return std::nullopt;
            }
            const ValueId baseId = operands[0];
            const Value baseValue = graph.getValue(baseId);
            const std::span<const int64_t> shape = baseValue.shape();
            const int64_t baseWidth = baseValue.width();
            if (shape.empty() || baseWidth <= 0)
            {
                return std::nullopt;
            }
            int64_t elementCount = 1;
            for (const int64_t dim : shape)
            {
                if (dim <= 0 || elementCount > std::numeric_limits<int64_t>::max() / dim)
                {
                    return std::nullopt;
                }
                elementCount *= dim;
            }
            if (elementCount <= 0 || baseWidth % elementCount != 0)
            {
                return std::nullopt;
            }
            const std::size_t indexCount = operands.size() - 1;
            if (indexCount < 1 || indexCount > shape.size())
            {
                return std::nullopt;
            }
            SliceArrayGeometry geometry;
            geometry.elementWidth = baseWidth / elementCount;
            geometry.indexCount = indexCount;
            int64_t subVolume = geometry.elementWidth;
            for (std::size_t i = indexCount; i < shape.size(); ++i)
            {
                subVolume *= shape[static_cast<std::size_t>(i)];
            }
            geometry.subVolume = subVolume;
            if (subVolume <= 0 ||
                graph.getValue(results[0]).width() != subVolume)
            {
                return std::nullopt;
            }

            int64_t linearIndex = 0;
            for (std::size_t i = 0; i < indexCount; ++i)
            {
                const ValueId indexId = operands[i + 1];
                if (!indexId.valid())
                {
                    return std::nullopt; // dynamic: invalid index operand
                }
                auto indexValue = getConstIntValue(graph, indexId);
                if (!indexValue)
                {
                    return geometry; // dynamic index: whole-array interval
                }
                const int64_t dim = shape[i];
                if (*indexValue < 0 || *indexValue >= dim)
                {
                    return geometry; // out-of-bounds constant: implementation-defined, be conservative
                }
                linearIndex = linearIndex * dim + *indexValue;
            }
            const int64_t offset = linearIndex * subVolume;
            if (offset < 0 || offset > baseWidth - subVolume)
            {
                return geometry; // out of flat range: conservative
            }
            geometry.selectable = true;
            geometry.offset = offset;
            return geometry;
        }

        // Reusable scratch for dependsTransitivelyOn: generation-stamped dense
        // visited marks keyed by ValueId.index (valid within one graph, which
        // is all a single DFS ever sees), avoiding a fresh hash set per call.
        struct TransitiveDepScratch
        {
            std::vector<uint32_t> visitedStamp;
            std::vector<ValueId> stack;
            uint32_t epoch = 0;
        };

        bool dependsTransitivelyOn(const Graph &graph, ValueId valueId, ValueId targetId,
                                   TransitiveDepScratch &scratch)
        {
            if (!valueId.valid() || !targetId.valid())
            {
                return false;
            }
            ++scratch.epoch;
            if (scratch.epoch == 0)
            {
                std::fill(scratch.visitedStamp.begin(), scratch.visitedStamp.end(), 0u);
                scratch.epoch = 1;
            }
            const uint32_t epoch = scratch.epoch;
            auto stampOf = [&](ValueId v) -> uint32_t & {
                if (v.index >= scratch.visitedStamp.size())
                {
                    scratch.visitedStamp.resize(static_cast<std::size_t>(v.index) + 1, 0u);
                }
                return scratch.visitedStamp[v.index];
            };
            scratch.stack.clear();
            scratch.stack.push_back(valueId);
            while (!scratch.stack.empty())
            {
                const ValueId current = scratch.stack.back();
                scratch.stack.pop_back();
                if (!current.valid())
                {
                    continue;
                }
                if (current == targetId)
                {
                    return true;
                }
                uint32_t &stamp = stampOf(current);
                if (stamp == epoch)
                {
                    continue;
                }
                stamp = epoch;
                const OperationId defOpId = graph.valueDef(current);
                if (!defOpId.valid())
                {
                    continue;
                }
                for (ValueId operandId : graph.opOperands(defOpId))
                {
                    if (operandId.valid())
                    {
                        scratch.stack.push_back(operandId);
                    }
                }
            }
            return false;
        }

        bool rangeWithin(const BitRange &inner, const BitRange &outer)
        {
            return inner.low >= outer.low && inner.high <= outer.high;
        }

        bool rangesOverlap(const BitRange &lhs, const BitRange &rhs)
        {
            return !(lhs.high < rhs.low || rhs.high < lhs.low);
        }

        bool getFullRange(const Graph &graph, ValueId valueId, BitRange &out)
        {
            if (!valueId.valid())
            {
                return false;
            }
            const int64_t width = graph.valueWidth(valueId);
            if (width <= 0)
            {
                return false;
            }
            out.low = 0;
            out.high = width - 1;
            return true;
        }

        bool opTouchesScc(const Graph &graph, OperationId opId, const std::unordered_set<ValueId, ValueIdHash> &sccSet)
        {
            bool hasOperand = false;
            bool hasResult = false;
            for (ValueId operand : graph.opOperands(opId))
            {
                if (sccSet.find(operand) != sccSet.end())
                {
                    hasOperand = true;
                    break;
                }
            }
            for (ValueId result : graph.opResults(opId))
            {
                if (sccSet.find(result) != sccSet.end())
                {
                    hasResult = true;
                    break;
                }
            }
            return hasOperand && hasResult;
        }

        std::vector<OperationId> collectLoopOps(const Graph &graph, const std::vector<ValueId> &loopValues)
        {
            std::unordered_set<ValueId, ValueIdHash> sccSet(loopValues.begin(), loopValues.end());
            std::unordered_set<OperationId, OperationIdHash> ops;

            for (ValueId valueId : loopValues)
            {
                const OperationId def = graph.valueDef(valueId);
                if (def.valid() && opTouchesScc(graph, def, sccSet))
                {
                    ops.insert(def);
                }

                const Value value = graph.getValue(valueId);
                for (const auto &user : value.users())
                {
                    OperationId opId = user.operation;
                    if (!opId.valid())
                    {
                        continue;
                    }
                    if (opTouchesScc(graph, opId, sccSet))
                    {
                        ops.insert(opId);
                    }
                }
            }

            std::vector<OperationId> out(ops.begin(), ops.end());
            std::sort(out.begin(), out.end(), [](const OperationId &lhs, const OperationId &rhs) {
                if (lhs.index != rhs.index)
                {
                    return lhs.index < rhs.index;
                }
                if (lhs.generation != rhs.generation)
                {
                    return lhs.generation < rhs.generation;
                }
                if (lhs.graph.index != rhs.graph.index)
                {
                    return lhs.graph.index < rhs.graph.index;
                }
                return lhs.graph.generation < rhs.graph.generation;
            });
            return out;
        }

        void addRangeEdge(RangeSuccMap &succ,
                          RangeNodeSet &nodes,
                          const RangeNode &src,
                          const RangeNode &dst)
        {
            succ[src].push_back(dst);
            nodes.insert(src);
            nodes.insert(dst);
        }

        bool mapConcatRanges(const Graph &graph,
                             const Operation &op,
                             const std::unordered_set<ValueId, ValueIdHash> &loopSet,
                             RangeSuccMap &succ,
                             RangeNodeSet &nodes,
                             bool &uncertain)
        {
            const auto &operands = op.operands();
            const auto &results = op.results();
            if (operands.empty() || results.empty())
            {
                return true;
            }

            std::vector<int64_t> widths;
            widths.reserve(operands.size());
            int64_t totalWidth = 0;
            for (ValueId operand : operands)
            {
                if (!operand.valid())
                {
                    uncertain = true;
                    return false;
                }
                const int64_t width = graph.getValue(operand).width();
                if (width <= 0)
                {
                    uncertain = true;
                    return false;
                }
                widths.push_back(width);
                totalWidth += width;
            }
            if (totalWidth <= 0)
            {
                uncertain = true;
                return false;
            }

            for (ValueId resultId : results)
            {
                if (!resultId.valid())
                {
                    continue;
                }
                BitRange resultRange{};
                if (!getFullRange(graph, resultId, resultRange))
                {
                    uncertain = true;
                    return false;
                }
                const int64_t resultWidth = resultRange.high - resultRange.low + 1;
                if (resultWidth != totalWidth)
                {
                    uncertain = true;
                    return false;
                }

                int64_t cursor = totalWidth;
                for (std::size_t i = 0; i < operands.size(); ++i)
                {
                    const std::size_t slot = op.kind() == OperationKind::kArrayCreate ? operands.size() - 1 - i : i;
                    const int64_t width = widths[slot];
                    const int64_t hi = cursor - 1;
                    const int64_t lo = cursor - width;
                    cursor = lo;
                    ValueId operand = operands[slot];
                    if (loopSet.find(operand) == loopSet.end() ||
                        loopSet.find(resultId) == loopSet.end())
                    {
                        continue;
                    }
                    BitRange operandRange{};
                    if (!getFullRange(graph, operand, operandRange))
                    {
                        uncertain = true;
                        return false;
                    }
                    BitRange mapped{lo, hi};
                    addRangeEdge(succ, nodes, RangeNode{operand, operandRange}, RangeNode{resultId, mapped});
                }
            }
            return true;
        }

        bool buildRangeGraph(LoopInfo &loop,
                             const Graph &graph,
                             const CombLoopElimOptions &options,
                             RangeSuccMap &succ,
                             std::vector<RangeNode> &nodes,
                             bool &uncertain,
                             wolvrix::lib::grh::OperationId *bailOp = nullptr)
        {
            RangeNodeSet nodeSet;
            std::unordered_set<ValueId, ValueIdHash> loopSet(loop.loopValues.begin(), loop.loopValues.end());

            // stage-05: arrays read through a dynamic (or out-of-bounds)
            // kSliceArray index may touch any element, so for analysis the
            // base value collapses to a single whole-array interval. Collect
            // those bases up front so every range node of the same value is
            // normalized consistently below.
            std::unordered_set<ValueId, ValueIdHash> wholeIntervalValues;
            if (options.enableArrayFragmentation && options.fixDynamicArrayLoops)
            {
                for (OperationId opId : loop.loopOps)
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    const Operation op = graph.getOperation(opId);
                    if (op.kind() != OperationKind::kSliceArray)
                    {
                        continue;
                    }
                    auto geometry = getSliceArrayGeometry(graph, op);
                    if (geometry && geometry->selectable)
                    {
                        continue;
                    }
                    const auto &operands = op.operands();
                    const auto &results = op.results();
                    if (operands.size() >= 2 && results.size() == 1 &&
                        loopSet.find(operands[0]) != loopSet.end() &&
                        loopSet.find(results[0]) != loopSet.end())
                    {
                        wholeIntervalValues.insert(operands[0]);
                    }
                }
            }

            for (OperationId opId : loop.loopOps)
            {
                if (!opId.valid())
                {
                    continue;
                }
                if (bailOp)
                {
                    *bailOp = opId;
                }
                const Operation op = graph.getOperation(opId);
                // stage-07: a dynamic register-array read is a state-content
                // boundary, but its index is an ordinary combinational input.
                // Keep the index→result edge visible so false loops that merely
                // share the index cone reach the range SCC instead of hiding
                // behind the boundary classification (the state content itself
                // never participates in the graph).
                if (isBoundaryOp(op.kind()) &&
                    op.kind() != OperationKind::kRegisterReadArrayPort)
                {
                    continue;
                }
                const auto &operands = op.operands();
                const auto &results = op.results();
                if (operands.empty() || results.empty())
                {
                    continue;
                }

                switch (op.kind())
                {
                case OperationKind::kRegisterReadArrayPort:
                {
                    if (operands.size() != 1 || results.size() != 1)
                    {
                        uncertain = true;
                        return false;
                    }
                    const ValueId index = operands[0];
                    const ValueId resultId = results[0];
                    if (!index.valid() || !resultId.valid())
                    {
                        uncertain = true;
                        return false;
                    }
                    if (loopSet.find(index) != loopSet.end() &&
                        loopSet.find(resultId) != loopSet.end())
                    {
                        BitRange indexRange{};
                        BitRange resultRange{};
                        if (!getFullRange(graph, index, indexRange) ||
                            !getFullRange(graph, resultId, resultRange))
                        {
                            uncertain = true;
                            return false;
                        }
                        addRangeEdge(succ, nodeSet, RangeNode{index, indexRange},
                                     RangeNode{resultId, resultRange});
                    }
                    break;
                }
                case OperationKind::kSliceStatic:
                {
                    if (operands.size() != 1 || results.size() != 1)
                    {
                        uncertain = true;
                        return false;
                    }
                    ValueId base = operands[0];
                    ValueId resultId = results[0];
                    if (!base.valid() || !resultId.valid())
                    {
                        uncertain = true;
                        return false;
                    }
                    auto startOpt = getIntAttr(op, "sliceStart");
                    auto endOpt = getIntAttr(op, "sliceEnd");
                    if (!startOpt || !endOpt)
                    {
                        uncertain = true;
                        return false;
                    }
                    BitRange baseRange{};
                    BitRange resultRange{};
                    if (!getFullRange(graph, base, baseRange) ||
                        !getFullRange(graph, resultId, resultRange))
                    {
                        uncertain = true;
                        return false;
                    }
                    BitRange sliceRange{std::min(*startOpt, *endOpt), std::max(*startOpt, *endOpt)};
                    if (sliceRange.low < 0 ||
                        !rangeWithin(sliceRange, baseRange))
                    {
                        uncertain = true;
                        return false;
                    }
                    const int64_t sliceWidth = sliceRange.high - sliceRange.low + 1;
                    const int64_t resultWidth = resultRange.high - resultRange.low + 1;
                    if (sliceWidth != resultWidth)
                    {
                        uncertain = true;
                        return false;
                    }
                    if (loopSet.find(base) != loopSet.end() &&
                        loopSet.find(resultId) != loopSet.end())
                    {
                        addRangeEdge(succ, nodeSet, RangeNode{base, sliceRange}, RangeNode{resultId, resultRange});
                    }
                    break;
                }
                case OperationKind::kConcat:
                case OperationKind::kArrayCreate:
                    if (!mapConcatRanges(graph, op, loopSet, succ, nodeSet, uncertain))
                    {
                        return false;
                    }
                    break;
                case OperationKind::kSliceDynamic:
                {
                    if (operands.size() < 2 || results.size() != 1)
                    {
                        uncertain = true;
                        return false;
                    }
                    ValueId base = operands[0];
                    ValueId index = operands[1];
                    ValueId resultId = results[0];
                    if (!base.valid() || !index.valid() || !resultId.valid())
                    {
                        uncertain = true;
                        return false;
                    }
                    auto widthOpt = getIntAttr(op, "sliceWidth");
                    auto indexOpt = getConstIntValue(graph, index);
                    if (!widthOpt)
                    {
                        uncertain = true;
                        return false;
                    }
                    BitRange baseRange{};
                    BitRange resultRange{};
                    if (!getFullRange(graph, base, baseRange) ||
                        !getFullRange(graph, resultId, resultRange))
                    {
                        uncertain = true;
                        return false;
                    }
                    if (!indexOpt)
                    {
                        // Dynamic index: the selected window is unknown, so model
                        // the read conservatively as depending on the whole base
                        // (and on the index value itself) instead of giving up on
                        // the range graph. This mirrors the kSliceArray dynamic
                        // read handling below and lets the range SCC decide
                        // whether the cycle is real.
                        const bool baseInLoop = loopSet.find(base) != loopSet.end();
                        const bool resultInLoop = loopSet.find(resultId) != loopSet.end();
                        if (baseInLoop && resultInLoop)
                        {
                            addRangeEdge(succ, nodeSet, RangeNode{base, baseRange},
                                         RangeNode{resultId, resultRange});
                        }
                        if (resultInLoop && loopSet.find(index) != loopSet.end())
                        {
                            BitRange indexRange{};
                            if (!getFullRange(graph, index, indexRange))
                            {
                                uncertain = true;
                                return false;
                            }
                            addRangeEdge(succ, nodeSet, RangeNode{index, indexRange},
                                         RangeNode{resultId, resultRange});
                        }
                        break;
                    }
                    const int64_t sliceWidth = *widthOpt;
                    if (sliceWidth <= 0)
                    {
                        uncertain = true;
                        return false;
                    }
                    const int64_t start = *indexOpt;
                    const int64_t end = start + sliceWidth - 1;
                    BitRange sliceRange{start, end};
                    if (sliceRange.low < 0 || sliceRange.high < sliceRange.low ||
                        !rangeWithin(sliceRange, baseRange))
                    {
                        uncertain = true;
                        return false;
                    }
                    const int64_t resultWidth = resultRange.high - resultRange.low + 1;
                    if (sliceWidth != resultWidth)
                    {
                        uncertain = true;
                        return false;
                    }
                    if (loopSet.find(base) != loopSet.end() &&
                        loopSet.find(resultId) != loopSet.end())
                    {
                        addRangeEdge(succ, nodeSet, RangeNode{base, sliceRange}, RangeNode{resultId, resultRange});
                    }
                    break;
                }
                case OperationKind::kSliceArray:
                {
                    // stage-05: array element reads are no longer an analysis
                    // black hole. Constant in-bounds indices map to an exact
                    // row-major bit range of the base; a dynamic index is
                    // modeled conservatively as a whole-array interval.
                    if (!options.enableArrayFragmentation)
                    {
                        uncertain = true;
                        return false;
                    }
                    auto geometry = getSliceArrayGeometry(graph, op);
                    if (!geometry)
                    {
                        uncertain = true;
                        return false;
                    }
                    ValueId base = operands[0];
                    ValueId resultId = results[0];
                    const bool baseInLoop = loopSet.find(base) != loopSet.end();
                    const bool resultInLoop = loopSet.find(resultId) != loopSet.end();
                    if (!geometry->selectable)
                    {
                        if (baseInLoop && resultInLoop)
                        {
                            loop.hasDynamicArrayRead = true;
                            if (!options.fixDynamicArrayLoops)
                            {
                                uncertain = true;
                                return false;
                            }
                            BitRange baseRange{};
                            BitRange resultRange{};
                            if (!getFullRange(graph, base, baseRange) ||
                                !getFullRange(graph, resultId, resultRange))
                            {
                                uncertain = true;
                                return false;
                            }
                            addRangeEdge(succ, nodeSet, RangeNode{base, baseRange},
                                         RangeNode{resultId, resultRange});
                            // The selected element depends on the index value
                            // itself; keep that dependency visible.
                            for (std::size_t i = 1; i < operands.size(); ++i)
                            {
                                const ValueId indexId = operands[i];
                                if (!indexId.valid() ||
                                    loopSet.find(indexId) == loopSet.end())
                                {
                                    continue;
                                }
                                BitRange indexRange{};
                                if (!getFullRange(graph, indexId, indexRange))
                                {
                                    uncertain = true;
                                    return false;
                                }
                                addRangeEdge(succ, nodeSet, RangeNode{indexId, indexRange},
                                             RangeNode{resultId, resultRange});
                            }
                        }
                        break;
                    }
                    if (baseInLoop && resultInLoop)
                    {
                        BitRange baseRange{};
                        BitRange resultRange{};
                        if (!getFullRange(graph, base, baseRange) ||
                            !getFullRange(graph, resultId, resultRange))
                        {
                            uncertain = true;
                            return false;
                        }
                        BitRange sliceRange{geometry->offset,
                                            geometry->offset + geometry->subVolume - 1};
                        if (!rangeWithin(sliceRange, baseRange))
                        {
                            uncertain = true;
                            return false;
                        }
                        addRangeEdge(succ, nodeSet, RangeNode{base, sliceRange},
                                     RangeNode{resultId, resultRange});
                    }
                    break;
                }
                case OperationKind::kAssign:
                {
                    if (operands.size() != 1 || results.size() != 1)
                    {
                        uncertain = true;
                        return false;
                    }
                    ValueId operandId = operands[0];
                    ValueId resultId = results[0];
                    if (!operandId.valid() || !resultId.valid())
                    {
                        uncertain = true;
                        return false;
                    }
                    if (loopSet.find(operandId) == loopSet.end() ||
                        loopSet.find(resultId) == loopSet.end())
                    {
                        break;
                    }
                    BitRange operandRange{};
                    BitRange resultRange{};
                    if (!getFullRange(graph, operandId, operandRange) ||
                        !getFullRange(graph, resultId, resultRange))
                    {
                        uncertain = true;
                        return false;
                    }
                    const int64_t operandWidth = operandRange.high - operandRange.low + 1;
                    const int64_t resultWidth = resultRange.high - resultRange.low + 1;
                    if (operandWidth != resultWidth)
                    {
                        uncertain = true;
                        return false;
                    }
                    addRangeEdge(succ, nodeSet, RangeNode{operandId, operandRange}, RangeNode{resultId, resultRange});
                    break;
                }
                default:
                {
                    for (ValueId resultId : results)
                    {
                        if (!resultId.valid())
                        {
                            continue;
                        }
                        if (loopSet.find(resultId) == loopSet.end())
                        {
                            continue;
                        }
                        BitRange resultRange{};
                        if (!getFullRange(graph, resultId, resultRange))
                        {
                            uncertain = true;
                            return false;
                        }
                        for (ValueId operandId : operands)
                        {
                            if (!operandId.valid())
                            {
                                continue;
                            }
                            if (loopSet.find(operandId) == loopSet.end())
                            {
                                continue;
                            }
                            BitRange operandRange{};
                            if (!getFullRange(graph, operandId, operandRange))
                            {
                                uncertain = true;
                                return false;
                            }
                            addRangeEdge(succ, nodeSet,
                                         RangeNode{operandId, operandRange},
                                         RangeNode{resultId, resultRange});
                        }
                    }
                    break;
                }
                }
            }

            if (!wholeIntervalValues.empty())
            {
                // Collapse every range node of a dynamically-read array to its
                // whole-value interval so the SCC check sees the conservative
                // dependency as one node.
                auto normalizeNode = [&](const RangeNode &node) -> RangeNode
                {
                    if (wholeIntervalValues.find(node.value) == wholeIntervalValues.end())
                    {
                        return node;
                    }
                    BitRange full{};
                    if (!getFullRange(graph, node.value, full))
                    {
                        return node;
                    }
                    return RangeNode{node.value, full};
                };
                RangeSuccMap normalizedSucc;
                normalizedSucc.reserve(succ.size());
                RangeNodeSet normalizedNodes;
                for (const auto &entry : succ)
                {
                    const RangeNode src = normalizeNode(entry.first);
                    auto &dstList = normalizedSucc[src];
                    normalizedNodes.insert(src);
                    for (const RangeNode &dst : entry.second)
                    {
                        const RangeNode normalizedDst = normalizeNode(dst);
                        dstList.push_back(normalizedDst);
                        normalizedNodes.insert(normalizedDst);
                    }
                }
                succ = std::move(normalizedSucc);
                nodeSet = std::move(normalizedNodes);
            }

            nodes.assign(nodeSet.begin(), nodeSet.end());
            return true;
        }

        bool hasOverlapOnSameValue(const RangeScc &scc)
        {
            std::unordered_map<ValueId, std::vector<BitRange>, ValueIdHash> rangesByValue;
            for (const auto &node : scc.nodes)
            {
                rangesByValue[node.value].push_back(node.range);
            }
            for (auto &entry : rangesByValue)
            {
                auto &ranges = entry.second;
                if (ranges.size() < 2)
                {
                    continue;
                }
                std::sort(ranges.begin(), ranges.end(),
                          [](const BitRange &lhs, const BitRange &rhs) {
                              if (lhs.low != rhs.low)
                              {
                                  return lhs.low < rhs.low;
                              }
                              return lhs.high < rhs.high;
                          });
                BitRange prev = ranges.front();
                for (std::size_t i = 1; i < ranges.size(); ++i)
                {
                    if (rangesOverlap(prev, ranges[i]))
                    {
                        return true;
                    }
                    if (ranges[i].high > prev.high)
                    {
                        prev = ranges[i];
                    }
                }
            }
            return false;
        }

        bool classifyFalseLoop(LoopInfo &loop,
                               const Graph &graph,
                               const CombLoopElimOptions &options,
                               bool &analysisIncomplete)
        {
            static const bool debugLoops = std::getenv("WOLVRIX_CLE_DEBUG") != nullptr;
            RangeSuccMap rangeSucc;
            std::vector<RangeNode> rangeNodes;
            bool uncertain = false;
            wolvrix::lib::grh::OperationId bailOp{};
            if (!buildRangeGraph(loop, graph, options, rangeSucc, rangeNodes, uncertain, &bailOp))
            {
                analysisIncomplete = true;
                if (debugLoops)
                {
                    std::string bailKind = "none";
                    std::string bailSym = "none";
                    if (bailOp.valid())
                    {
                        const Operation op = graph.getOperation(bailOp);
                        bailKind = std::string(toString(op.kind()));
                        bailSym = std::string(op.symbolText());
                    }
                    std::fprintf(stderr, "[cle-debug] loop ops=%zu: range graph build failed at %s kind=%s\n",
                                 loop.loopOps.size(), bailSym.c_str(), bailKind.c_str());
                }
                return false;
            }
            if (uncertain)
            {
                analysisIncomplete = true;
                if (debugLoops)
                {
                    std::fprintf(stderr, "[cle-debug] loop ops=%zu: uncertain during range build\n",
                                 loop.loopOps.size());
                }
                return false;
            }
            if (rangeNodes.empty())
            {
                return true;
            }
            std::vector<RangeScc> rangeSccs = tarjanScc(rangeNodes, rangeSucc);
            for (const auto &scc : rangeSccs)
            {
                if (scc.nodes.empty())
                {
                    continue;
                }
                if (scc.nodes.size() == 1 && !hasSelfLoop(scc.nodes.front(), rangeSucc))
                {
                    continue;
                }
                if (debugLoops)
                {
                    std::fprintf(stderr, "[cle-debug] loop ops=%zu: range-level SCC size=%zu dynRead=%d\n",
                                 loop.loopOps.size(), scc.nodes.size(),
                                 loop.hasDynamicArrayRead ? 1 : 0);
                }
                if (scc.nodes.size() == 1 && hasSelfLoop(scc.nodes.front(), rangeSucc))
                {
                    return false;
                }
                if (loop.hasDynamicArrayRead)
                {
                    // A dynamic array read may observe any element of its
                    // base, so a residual range-level cycle is undecidable
                    // rather than a fixable false loop.
                    return false;
                }
                if (hasOverlapOnSameValue(scc))
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<SrcLoc> findLoopSrcLoc(const Graph &graph,
                                             const std::vector<ValueId> &loopValues,
                                             const std::vector<OperationId> &loopOps)
        {
            for (ValueId valueId : loopValues)
            {
                const Value value = graph.getValue(valueId);
                if (value.srcLoc())
                {
                    return value.srcLoc();
                }
            }
            for (OperationId opId : loopOps)
            {
                const Operation op = graph.getOperation(opId);
                if (op.srcLoc())
                {
                    return op.srcLoc();
                }
            }
            return std::nullopt;
        }

        std::string formatSrcLocShort(const std::optional<SrcLoc> &srcLoc)
        {
            if (!srcLoc)
            {
                return "none";
            }
            if (!srcLoc->file.empty())
            {
                std::string out = srcLoc->file;
                if (srcLoc->line != 0)
                {
                    out.push_back(':');
                    out += std::to_string(srcLoc->line);
                    if (srcLoc->column != 0)
                    {
                        out.push_back(':');
                        out += std::to_string(srcLoc->column);
                    }
                }
                return out;
            }
            std::string out;
            if (!srcLoc->origin.empty())
            {
                out.append(srcLoc->origin);
            }
            if (!srcLoc->pass.empty())
            {
                if (!out.empty())
                {
                    out.push_back(':');
                }
                out.append(srcLoc->pass);
            }
            if (!srcLoc->note.empty())
            {
                if (!out.empty())
                {
                    out.push_back(':');
                }
                out.append(srcLoc->note);
            }
            if (out.empty())
            {
                return "unknown";
            }
            return out;
        }

        std::string formatValueShort(const Graph &graph, ValueId valueId)
        {
            const Value value = graph.getValue(valueId);
            if (!value.symbolText().empty())
            {
                return std::string(value.symbolText());
            }
            return "value#" + std::to_string(valueId.index);
        }

        std::string formatOperationShort(const Graph &graph, OperationId opId)
        {
            const Operation op = graph.getOperation(opId);
            std::string name;
            if (!op.symbolText().empty())
            {
                name = std::string(op.symbolText());
            }
            else
            {
                name = "op#" + std::to_string(opId.index);
            }
            name.append("(");
            name.append(std::string(wolvrix::lib::grh::toString(op.kind())));
            name.append(")");
            return name;
        }

        std::string describeLoop(const LoopInfo &loop,
                                 const Graph &graph,
                                 const std::optional<SrcLoc> &srcLoc,
                                 std::string_view status,
                                 std::string_view analysisNote)
        {
            std::string desc = "comb loop detected (values=";
            desc.append(std::to_string(loop.loopValues.size()));
            desc.append(", ops=");
            desc.append(std::to_string(loop.loopOps.size()));
            desc.append(")");
            desc.append(" srcloc=");
            desc.append(formatSrcLocShort(srcLoc));
            if (!status.empty())
            {
                desc.append(" status=");
                desc.append(status);
            }
            if (!analysisNote.empty())
            {
                desc.append(" ");
                desc.append(analysisNote);
            }

            desc.append("\n  values:");
            if (loop.loopValues.empty())
            {
                desc.append("\n    - <none>");
            }
            for (ValueId valueId : loop.loopValues)
            {
                const Value value = graph.getValue(valueId);
                desc.append("\n    - ");
                desc.append(formatValueShort(graph, valueId));
                desc.append(" id=");
                desc.append(std::to_string(valueId.index));
                desc.append(" src=");
                desc.append(formatSrcLocShort(value.srcLoc()));
            }

            desc.append("\n  ops:");
            if (loop.loopOps.empty())
            {
                desc.append("\n    - <none>");
            }
            for (OperationId opId : loop.loopOps)
            {
                if (!opId.valid())
                {
                    desc.append("\n    - <invalid>");
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                desc.append("\n    - ");
                desc.append(formatOperationShort(graph, opId));
                desc.append(" id=");
                desc.append(std::to_string(opId.index));
                desc.append(" src=");
                desc.append(formatSrcLocShort(op.srcLoc()));
            }

            return desc;
        }

        std::vector<LoopInfo> collectLoopInfos(const Graph &graph,
                                               const std::span<const ValueId> values,
                                               const DenseSucc &succ)
        {
            std::vector<LoopInfo> loops;
            std::vector<Scc> sccs = tarjanScc(values, succ);
            for (const auto &scc : sccs)
            {
                if (scc.nodes.empty())
                {
                    continue;
                }
                if (scc.nodes.size() == 1 && !hasSelfLoop(scc.nodes.front(), succ))
                {
                    continue;
                }
                LoopInfo loop;
                loop.loopValues = scc.nodes;
                std::sort(loop.loopValues.begin(), loop.loopValues.end(), [](const ValueId &lhs, const ValueId &rhs) {
                    if (lhs.index != rhs.index)
                    {
                        return lhs.index < rhs.index;
                    }
                    if (lhs.generation != rhs.generation)
                    {
                        return lhs.generation < rhs.generation;
                    }
                    if (lhs.graph.index != rhs.graph.index)
                    {
                        return lhs.graph.index < rhs.graph.index;
                    }
                    return lhs.graph.generation < rhs.graph.generation;
                });
                loop.loopOps = collectLoopOps(graph, loop.loopValues);
                loops.push_back(std::move(loop));
            }
            return loops;
        }

        [[maybe_unused]] bool retargetSliceOp(Graph &graph,
                             OperationId opId,
                             const std::unordered_set<ValueId, ValueIdHash> &loopSet,
                             std::size_t &retargeted,
                             const std::function<void(const Operation &, std::string)> &onError,
                             TransitiveDepScratch &depScratch)
        {
            const OperationKind opKind = graph.opKind(opId);
            if (opKind != OperationKind::kSliceStatic &&
                opKind != OperationKind::kSliceDynamic &&
                opKind != OperationKind::kSliceArray)
            {
                return false;
            }
            const auto operands = graph.opOperands(opId);
            const auto results = graph.opResults(opId);
            if (results.size() != 1)
            {
                return false;
            }
            const ValueId resultId = results[0];
            if (!resultId.valid())
            {
                return false;
            }
            if (opKind == OperationKind::kSliceStatic && operands.size() != 1)
            {
                return false;
            }
            if (opKind == OperationKind::kSliceDynamic && (operands.size() < 2 || !operands[1].valid()))
            {
                return false;
            }
            std::optional<SliceArrayGeometry> arrayGeometry;
            if (opKind == OperationKind::kSliceArray)
            {
                arrayGeometry = getSliceArrayGeometry(graph, graph.getOperation(opId));
                // A dynamic or otherwise non-selectable array read cannot be
                // retargeted through a structural alias: doing so would
                // preserve the alias but could change which array element is
                // selected.
                if (!arrayGeometry || !arrayGeometry->selectable)
                {
                    return false;
                }
            }
            const ValueId base = operands[0];
            if (!base.valid())
            {
                return false;
            }
            // The first retarget may move an array read through an alias whose
            // replacement base is outside the SCC. Keep walking that same
            // entrance while its result remains in the loop so an
            // assign->concat chain can be reduced all the way to the selected
            // concat operand.
            if (loopSet.find(base) == loopSet.end() &&
                (opKind != OperationKind::kSliceArray ||
                 loopSet.find(resultId) == loopSet.end()))
            {
                return false;
            }
            int64_t low = 0;
            int64_t high = -1;
            if (opKind == OperationKind::kSliceStatic)
            {
                auto startOpt = getIntAttr(graph, opId, "sliceStart");
                auto endOpt = getIntAttr(graph, opId, "sliceEnd");
                if (!startOpt || !endOpt)
                {
                    return false;
                }
                low = *startOpt;
                high = *endOpt;
            }
            else if (opKind == OperationKind::kSliceDynamic)
            {
                auto widthOpt = getIntAttr(graph, opId, "sliceWidth");
                auto indexOpt = getConstIntValue(graph, operands[1]);
                if (!widthOpt || !indexOpt)
                {
                    return false;
                }
                low = *indexOpt;
                high = low + *widthOpt - 1;
            }
            else
            {
                low = arrayGeometry->offset;
                high = low + arrayGeometry->subVolume - 1;
            }
            if (low < 0 || high < low)
            {
                return false;
            }

            const OperationId baseDefId = graph.valueDef(base);
            if (!baseDefId.valid())
            {
                return false;
            }
            const OperationKind baseDefKind = graph.opKind(baseDefId);

            auto foldArrayReadInFalseLoop = [&]() {
                if (opKind != OperationKind::kSliceArray ||
                    loopSet.find(base) == loopSet.end() ||
                    loopSet.find(resultId) == loopSet.end())
                {
                    return false;
                }
                try
                {
                    while (graph.opOperands(opId).size() > 1)
                    {
                        graph.eraseOperand(opId, graph.opOperands(opId).size() - 1);
                    }
                    graph.setOpKind(opId, OperationKind::kSliceStatic);
                    graph.setAttr(opId, "sliceStart", low);
                    graph.setAttr(opId, "sliceEnd", high);
                }
                catch (const std::exception &ex)
                {
                    onError(graph.getOperation(opId), std::string("Failed to fold false-loop array read: ") + ex.what());
                    return false;
                }
                ++retargeted;
                return true;
            };

            if (baseDefKind == OperationKind::kAssign)
            {
                const auto baseOps = graph.opOperands(baseDefId);
                if (baseOps.size() != 1)
                {
                    return false;
                }
                const ValueId newBase = baseOps[0];
                if (!newBase.valid() || newBase == base)
                {
                    return false;
                }
                BitRange baseRange{};
                BitRange newBaseRange{};
                if (!getFullRange(graph, base, baseRange) ||
                    !getFullRange(graph, newBase, newBaseRange))
                {
                    return false;
                }
                const int64_t baseWidth = baseRange.high - baseRange.low + 1;
                const int64_t newWidth = newBaseRange.high - newBaseRange.low + 1;
                if (baseWidth != newWidth)
                {
                    return false;
                }
                if (opKind == OperationKind::kSliceArray)
                {
                    const auto baseShape = graph.valueShape(base);
                    const auto newBaseShape = graph.valueShape(newBase);
                    if (baseShape.size() != newBaseShape.size() ||
                        !std::equal(baseShape.begin(), baseShape.end(), newBaseShape.begin()))
                    {
                        // The alias changes the array view, so keep the base
                        // and select its exact bit range before retargeting.
                        return foldArrayReadInFalseLoop();
                    }
                }
                try
                {
                    graph.replaceOperand(opId, 0, newBase);
                }
                catch (const std::exception &ex)
                {
                    onError(graph.getOperation(baseDefId), std::string("Failed to retarget slice through kAssign: ") + ex.what());
                    return false;
                }
                ++retargeted;
                return true;
            }

            if (baseDefKind == OperationKind::kSliceStatic &&
                opKind != OperationKind::kSliceArray)
            {
                const auto baseOps = graph.opOperands(baseDefId);
                if (baseOps.size() != 1)
                {
                    return false;
                }
                auto baseStart = getIntAttr(graph, baseDefId, "sliceStart");
                auto baseEnd = getIntAttr(graph, baseDefId, "sliceEnd");
                if (!baseStart || !baseEnd)
                {
                    return false;
                }
                const int64_t baseLow = *baseStart;
                const int64_t baseHigh = *baseEnd;
                if (baseLow < 0 || baseHigh < baseLow)
                {
                    return false;
                }
                const ValueId newBase = baseOps[0];
                if (!newBase.valid())
                {
                    return false;
                }
                if (dependsTransitivelyOn(graph, newBase, resultId, depScratch))
                {
                    return false;
                }
                const int64_t newLow = baseLow + low;
                const int64_t newHigh = baseLow + high;
                BitRange newBaseRange{};
                if (!getFullRange(graph, newBase, newBaseRange))
                {
                    return false;
                }
                if (newLow < 0 || newHigh < newLow || newHigh > newBaseRange.high)
                {
                    return false;
                }
                bool changed = false;
                try
                {
                    if (newBase != base)
                    {
                        graph.replaceOperand(opId, 0, newBase);
                        changed = true;
                    }
                    // A constant dynamic index was relative to the old
                    // slice. Preserve the combined offset by making this a
                    // static slice; sliceStart/End do not affect a dynamic
                    // slice while its old index operand remains attached.
                    if (opKind == OperationKind::kSliceDynamic)
                    {
                        graph.setOpKind(opId, OperationKind::kSliceStatic);
                        while (graph.opOperands(opId).size() > 1)
                        {
                            graph.eraseOperand(opId, graph.opOperands(opId).size() - 1);
                        }
                        graph.eraseAttr(opId, "sliceWidth");
                        changed = true;
                    }
                    if (newLow != low || opKind == OperationKind::kSliceDynamic)
                    {
                        graph.setAttr(opId, "sliceStart", newLow);
                        changed = true;
                    }
                    if (newHigh != high || opKind == OperationKind::kSliceDynamic)
                    {
                        graph.setAttr(opId, "sliceEnd", newHigh);
                        changed = true;
                    }
                }
                catch (const std::exception &ex)
                {
                    onError(graph.getOperation(baseDefId), std::string("Failed to retarget slice through kSliceStatic: ") + ex.what());
                    return false;
                }
                if (changed)
                {
                    ++retargeted;
                }
                return changed;
            }

            if (baseDefKind == OperationKind::kConcat)
            {
                const auto concatOps = graph.opOperands(baseDefId);
                if (concatOps.empty())
                {
                    return false;
                }
                std::vector<int64_t> widths;
                widths.reserve(concatOps.size());
                int64_t totalWidth = 0;
                for (ValueId operandId : concatOps)
                {
                    if (!operandId.valid())
                    {
                        return false;
                    }
                    const int64_t width = graph.valueWidth(operandId);
                    if (width <= 0)
                    {
                        return false;
                    }
                    widths.push_back(width);
                    totalWidth += width;
                }
                if (totalWidth <= 0)
                {
                    return false;
                }
                BitRange baseRange{};
                if (!getFullRange(graph, base, baseRange))
                {
                    return false;
                }
                const int64_t baseWidth = baseRange.high - baseRange.low + 1;
                if (baseWidth != totalWidth)
                {
                    return false;
                }
                if (high >= totalWidth)
                {
                    return false;
                }

                int64_t cursor = totalWidth;
                for (std::size_t i = 0; i < concatOps.size(); ++i)
                {
                    const int64_t width = widths[i];
                    const int64_t opHigh = cursor - 1;
                    const int64_t opLow = cursor - width;
                    cursor = opLow;
                    if (low < opLow || high > opHigh)
                    {
                        continue;
                    }
                    const int64_t newLow = low - opLow;
                    const int64_t newHigh = high - opLow;
                    const ValueId newBase = concatOps[i];
                    if (!newBase.valid())
                    {
                        return false;
                    }
                    // A constant array read already depends on this concat
                    // operand. Selecting it directly preserves the same
                    // dependency even when a different bit range feeds back.
                    if (newBase == resultId ||
                        (opKind != OperationKind::kSliceArray &&
                         dependsTransitivelyOn(graph, newBase, resultId, depScratch)))
                    {
                        return false;
                    }
                    bool changed = false;
                    try
                    {
                        if (newBase != base)
                        {
                            graph.replaceOperand(opId, 0, newBase);
                            changed = true;
                        }
                        const int64_t operandWidth = graph.valueWidth(newBase);
                        if (newLow == 0 && newHigh + 1 == operandWidth)
                        {
                            graph.setOpKind(opId, OperationKind::kAssign);
                            while (graph.opOperands(opId).size() > 1)
                            {
                                graph.eraseOperand(opId, graph.opOperands(opId).size() - 1);
                            }
                            graph.eraseAttr(opId, "sliceStart");
                            graph.eraseAttr(opId, "sliceEnd");
                            graph.eraseAttr(opId, "sliceWidth");
                            changed = true;
                        }
                        else if (opKind == OperationKind::kSliceStatic)
                        {
                            if (newLow != low)
                            {
                                graph.setAttr(opId, "sliceStart", newLow);
                                changed = true;
                            }
                            if (newHigh != high)
                            {
                                graph.setAttr(opId, "sliceEnd", newHigh);
                                changed = true;
                            }
                        }
                        else
                        {
                            graph.setOpKind(opId, OperationKind::kSliceStatic);
                            while (graph.opOperands(opId).size() > 1)
                            {
                                graph.eraseOperand(opId, graph.opOperands(opId).size() - 1);
                            }
                            graph.setAttr(opId, "sliceStart", newLow);
                            graph.setAttr(opId, "sliceEnd", newHigh);
                            graph.eraseAttr(opId, "sliceWidth");
                            changed = true;
                        }
                    }
                    catch (const std::exception &ex)
                    {
                        onError(graph.getOperation(baseDefId), std::string("Failed to retarget slice through kConcat: ") + ex.what());
                        return false;
                    }
                    if (changed)
                    {
                        ++retargeted;
                    }
                    return changed;
                }
            }

            return foldArrayReadInFalseLoop();
        }

        bool retargetLoopSliceEntrances(Graph &graph,
                                        const LoopInfo &loop,
                                        std::size_t &retargeted,
                                        const std::function<void(const Operation &, std::string)> &onError)
        {
            std::unordered_set<ValueId, ValueIdHash> loopSet(loop.loopValues.begin(), loop.loopValues.end());
            TransitiveDepScratch depScratch;
            bool changed = false;
            bool localChanged = true;
            while (localChanged)
            {
                localChanged = false;
                for (OperationId opId : loop.loopOps)
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    if (retargetSliceOp(graph, opId, loopSet, retargeted, onError, depScratch))
                    {
                        changed = true;
                        localChanged = true;
                    }
                }
            }
            return changed;
        }

        struct MappingEdge
        {
            ValueId src;
            ValueId dst;
            int64_t srcLow = 0;
            int64_t dstLow = 0;
            int64_t width = 0;
        };

        bool isCompareOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kEq:
            case OperationKind::kNe:
            case OperationKind::kCaseEq:
            case OperationKind::kCaseNe:
            case OperationKind::kWildcardEq:
            case OperationKind::kWildcardNe:
            case OperationKind::kLt:
            case OperationKind::kLe:
            case OperationKind::kGt:
            case OperationKind::kGe:
                return true;
            default:
                return false;
            }
        }

        bool collectCopyEdges(const Graph &graph,
                              const std::vector<OperationId> &loopOps,
                              std::vector<MappingEdge> &edges)
        {
            edges.clear();
            for (OperationId opId : loopOps)
            {
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                const auto &operands = op.operands();
                const auto &results = op.results();
                if (results.size() != 1)
                {
                    return false;
                }
                const ValueId resultId = results[0];
                if (!resultId.valid())
                {
                    return false;
                }
                const int64_t resultWidth = graph.getValue(resultId).width();
                if (resultWidth <= 0)
                {
                    return false;
                }
                switch (op.kind())
                {
                case OperationKind::kAssign:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    const int64_t operandWidth = graph.getValue(operands[0]).width();
                    if (operandWidth != resultWidth)
                    {
                        return false;
                    }
                    edges.push_back(MappingEdge{operands[0], resultId, 0, 0, resultWidth});
                    break;
                }
                case OperationKind::kSliceStatic:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    auto startOpt = getIntAttr(op, "sliceStart");
                    auto endOpt = getIntAttr(op, "sliceEnd");
                    if (!startOpt || !endOpt)
                    {
                        return false;
                    }
                    const int64_t sliceLow = *startOpt;
                    const int64_t sliceHigh = *endOpt;
                    if (sliceLow < 0 || sliceHigh < sliceLow)
                    {
                        return false;
                    }
                    const int64_t sliceWidth = sliceHigh - sliceLow + 1;
                    if (sliceWidth != resultWidth)
                    {
                        return false;
                    }
                    edges.push_back(MappingEdge{operands[0], resultId, sliceLow, 0, sliceWidth});
                    break;
                }
                case OperationKind::kSliceDynamic:
                {
                    if (operands.size() < 2 || !operands[0].valid() || !operands[1].valid())
                    {
                        return false;
                    }
                    auto widthOpt = getIntAttr(op, "sliceWidth");
                    auto indexOpt = getConstIntValue(graph, operands[1]);
                    if (!widthOpt || !indexOpt)
                    {
                        return false;
                    }
                    const int64_t sliceWidth = *widthOpt;
                    const int64_t sliceLow = *indexOpt;
                    const int64_t sliceHigh = sliceLow + sliceWidth - 1;
                    if (sliceWidth <= 0 || sliceLow < 0 || sliceHigh < sliceLow)
                    {
                        return false;
                    }
                    const int64_t operandWidth = graph.getValue(operands[0]).width();
                    if (sliceHigh >= operandWidth)
                    {
                        return false;
                    }
                    if (sliceWidth != resultWidth)
                    {
                        return false;
                    }
                    edges.push_back(MappingEdge{operands[0], resultId, sliceLow, 0, sliceWidth});
                    break;
                }
                case OperationKind::kSliceArray:
                {
                    // stage-05: constant in-bounds indices are a plain copy
                    // edge from the selected base range. A dynamic index has
                    // no static mapping; the op contributes no cut points so
                    // its result stays one whole fragment.
                    auto geometry = getSliceArrayGeometry(graph, op);
                    if (!geometry)
                    {
                        return false;
                    }
                    if (!geometry->selectable)
                    {
                        break;
                    }
                    edges.push_back(MappingEdge{operands[0], resultId, geometry->offset, 0,
                                                geometry->subVolume});
                    break;
                }
                case OperationKind::kConcat:
                case OperationKind::kArrayCreate:
                {
                    if (operands.empty())
                    {
                        return false;
                    }
                    std::vector<int64_t> widths;
                    widths.reserve(operands.size());
                    int64_t totalWidth = 0;
                    for (ValueId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            return false;
                        }
                        const int64_t width = graph.getValue(operand).width();
                        if (width <= 0)
                        {
                            return false;
                        }
                        widths.push_back(width);
                        totalWidth += width;
                    }
                    if (totalWidth != resultWidth)
                    {
                        return false;
                    }
                    int64_t cursor = totalWidth;
                    for (std::size_t i = 0; i < operands.size(); ++i)
                    {
                        const std::size_t slot = op.kind() == OperationKind::kArrayCreate ? operands.size() - 1 - i : i;
                        const int64_t width = widths[slot];
                        const int64_t hi = cursor - 1;
                        const int64_t lo = cursor - width;
                        cursor = lo;
                        edges.push_back(MappingEdge{operands[slot], resultId, 0, lo, width});
                    }
                    break;
                }
                case OperationKind::kReplicate:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    auto repOpt = getIntAttr(op, "rep");
                    if (!repOpt || *repOpt <= 0)
                    {
                        return false;
                    }
                    const int64_t operandWidth = graph.getValue(operands[0]).width();
                    if (operandWidth <= 0 ||
                        operandWidth > std::numeric_limits<int64_t>::max() / *repOpt ||
                        operandWidth * *repOpt != resultWidth)
                    {
                        return false;
                    }
                    for (int64_t i = 0; i < *repOpt; ++i)
                    {
                        edges.push_back(MappingEdge{operands[0], resultId, 0, i * operandWidth, operandWidth});
                    }
                    break;
                }
                case OperationKind::kAnd:
                case OperationKind::kOr:
                case OperationKind::kXor:
                case OperationKind::kXnor:
                case OperationKind::kLogicAnd:
                case OperationKind::kLogicOr:
                {
                    if (operands.size() < 2)
                    {
                        return false;
                    }
                    for (ValueId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            return false;
                        }
                        const int64_t operandWidth = graph.getValue(operand).width();
                        if (operandWidth != resultWidth)
                        {
                            return false;
                        }
                        edges.push_back(MappingEdge{operand, resultId, 0, 0, resultWidth});
                    }
                    break;
                }
                case OperationKind::kNot:
                case OperationKind::kLogicNot:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    const int64_t operandWidth = graph.getValue(operands[0]).width();
                    if (operandWidth != resultWidth)
                    {
                        return false;
                    }
                    edges.push_back(MappingEdge{operands[0], resultId, 0, 0, resultWidth});
                    break;
                }
                case OperationKind::kMux:
                {
                    if (operands.size() != 3 ||
                        !operands[0].valid() || !operands[1].valid() || !operands[2].valid())
                    {
                        return false;
                    }
                    if (graph.getValue(operands[0]).width() != 1)
                    {
                        return false;
                    }
                    const int64_t trueWidth = graph.getValue(operands[1]).width();
                    const int64_t falseWidth = graph.getValue(operands[2]).width();
                    if (trueWidth != resultWidth || falseWidth != resultWidth)
                    {
                        return false;
                    }
                    edges.push_back(MappingEdge{operands[1], resultId, 0, 0, resultWidth});
                    edges.push_back(MappingEdge{operands[2], resultId, 0, 0, resultWidth});
                    break;
                }
                default:
                    break;
                }
            }
            return true;
        }

        // `unsplittable` values (e.g. results of dynamic-index kSliceArray
        // reads) are defined monolithically and must stay a single
        // whole-value fragment; no cut points are ever inserted into them.
        bool buildLoopPartitions(const Graph &graph,
                                 const std::vector<ValueId> &loopValues,
                                 const std::vector<MappingEdge> &edges,
                                 const std::unordered_set<ValueId, ValueIdHash> &unsplittable,
                                 std::unordered_map<ValueId, std::vector<BitRange>, ValueIdHash> &segments)
        {
            std::unordered_set<ValueId, ValueIdHash> loopSet(loopValues.begin(), loopValues.end());
            std::unordered_map<ValueId, std::set<int64_t>, ValueIdHash> boundaries;
            boundaries.reserve(loopValues.size());
            for (ValueId valueId : loopValues)
            {
                const int64_t width = graph.getValue(valueId).width();
                if (width <= 0)
                {
                    return false;
                }
                auto &cuts = boundaries[valueId];
                cuts.insert(0);
                cuts.insert(width);
            }

            for (const auto &edge : edges)
            {
                if (loopSet.find(edge.dst) != loopSet.end() &&
                    unsplittable.find(edge.dst) == unsplittable.end())
                {
                    auto &cuts = boundaries[edge.dst];
                    cuts.insert(edge.dstLow);
                    cuts.insert(edge.dstLow + edge.width);
                }
                if (loopSet.find(edge.src) != loopSet.end() &&
                    unsplittable.find(edge.src) == unsplittable.end())
                {
                    auto &cuts = boundaries[edge.src];
                    cuts.insert(edge.srcLow);
                    cuts.insert(edge.srcLow + edge.width);
                }
            }

            bool changed = true;
            while (changed)
            {
                changed = false;
                for (const auto &edge : edges)
                {
                    const bool srcInLoop = loopSet.find(edge.src) != loopSet.end();
                    const bool dstInLoop = loopSet.find(edge.dst) != loopSet.end();
                    if (!srcInLoop && !dstInLoop)
                    {
                        continue;
                    }
                    if (srcInLoop && dstInLoop)
                    {
                        auto &srcCuts = boundaries[edge.src];
                        auto &dstCuts = boundaries[edge.dst];
                        const bool srcMutable = unsplittable.find(edge.src) == unsplittable.end();
                        const bool dstMutable = unsplittable.find(edge.dst) == unsplittable.end();
                        const int64_t srcEnd = edge.srcLow + edge.width;
                        const int64_t dstEnd = edge.dstLow + edge.width;
                        if (dstMutable)
                        {
                            for (auto it = srcCuts.lower_bound(edge.srcLow);
                                 it != srcCuts.end() && *it <= srcEnd; ++it)
                            {
                                const int64_t mapped = edge.dstLow + (*it - edge.srcLow);
                                if (dstCuts.insert(mapped).second)
                                {
                                    changed = true;
                                }
                            }
                        }
                        if (srcMutable)
                        {
                            for (auto it = dstCuts.lower_bound(edge.dstLow);
                                 it != dstCuts.end() && *it <= dstEnd; ++it)
                            {
                                const int64_t mapped = edge.srcLow + (*it - edge.dstLow);
                                if (srcCuts.insert(mapped).second)
                                {
                                    changed = true;
                                }
                            }
                        }
                    }
                }
            }

            segments.clear();
            segments.reserve(loopValues.size());
            for (ValueId valueId : loopValues)
            {
                const int64_t width = graph.getValue(valueId).width();
                auto it = boundaries.find(valueId);
                if (it == boundaries.end())
                {
                    return false;
                }
                auto &cuts = it->second;
                cuts.insert(0);
                cuts.insert(width);
                std::vector<int64_t> ordered(cuts.begin(), cuts.end());
                if (ordered.empty() || ordered.front() != 0 || ordered.back() != width)
                {
                    return false;
                }
                std::vector<BitRange> ranges;
                for (std::size_t i = 0; i + 1 < ordered.size(); ++i)
                {
                    const int64_t low = ordered[i];
                    const int64_t high = ordered[i + 1] - 1;
                    if (high < low)
                    {
                        continue;
                    }
                    ranges.push_back(BitRange{low, high});
                }
                if (ranges.empty())
                {
                    return false;
                }
                segments.emplace(valueId, std::move(ranges));
            }
            return true;
        }

        bool splitFalseLoop(Graph &graph,
                            const LoopInfo &loop,
                            const CombLoopElimOptions &options,
                            std::size_t &valuesSplit,
                            std::size_t &opsRewritten,
                            bool &hitFragmentCap,
                            const std::function<void(const Operation &, std::string)> &onError)
        {
            std::vector<MappingEdge> edges;
            if (!collectCopyEdges(graph, loop.loopOps, edges))
            {
                return false;
            }

            std::unordered_set<ValueId, ValueIdHash> loopSet(loop.loopValues.begin(), loop.loopValues.end());

            // stage-05: results of dynamic-index kSliceArray reads are defined
            // monolithically (the selected element is not statically known),
            // so they must stay one whole fragment. stage-07: the same holds
            // for monolithic loop ops (shifts, arithmetic, register-array
            // reads) — their results take no cut points.
            std::unordered_set<ValueId, ValueIdHash> unsplittable;
            for (OperationId opId : loop.loopOps)
            {
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                if (op.kind() == OperationKind::kSliceArray)
                {
                    const auto &results = op.results();
                    if (results.size() != 1 || !results[0].valid() ||
                        loopSet.find(results[0]) == loopSet.end())
                    {
                        continue;
                    }
                    auto geometry = getSliceArrayGeometry(graph, op);
                    if (!geometry || !geometry->selectable)
                    {
                        unsplittable.insert(results[0]);
                    }
                    continue;
                }
                if (isMonolithicLoopOp(op.kind()))
                {
                    for (ValueId resultValue : op.results())
                    {
                        if (resultValue.valid() &&
                            loopSet.find(resultValue) != loopSet.end())
                        {
                            unsplittable.insert(resultValue);
                        }
                    }
                }
            }

            std::unordered_map<ValueId, std::vector<BitRange>, ValueIdHash> segments;
            if (!buildLoopPartitions(graph, loop.loopValues, edges, unsplittable, segments))
            {
                return false;
            }

            hitFragmentCap = false;
            if (options.maxFragmentsPerValue > 0)
            {
                for (const auto &entry : segments)
                {
                    if (entry.second.size() > options.maxFragmentsPerValue)
                    {
                        hitFragmentCap = true;
                        return false;
                    }
                }
            }

            std::unordered_map<RangeNode, ValueId, RangeNodeHash> fragmentMap;
            fragmentMap.reserve(loop.loopValues.size() * 2);

            valuesSplit = 0;
            for (ValueId valueId : loop.loopValues)
            {
                const auto segIt = segments.find(valueId);
                if (segIt == segments.end())
                {
                    return false;
                }
                const Value baseValue = graph.getValue(valueId);
                if (segIt->second.size() > 1)
                {
                    ++valuesSplit;
                }
                for (const auto &range : segIt->second)
                {
                    const int64_t width = range.high - range.low + 1;
                    if (width <= 0)
                    {
                        return false;
                    }
                    // A fragment that still covers a shaped value as a whole
                    // keeps the array shape (e.g. as a kSliceArray result or
                    // base); sub-range fragments are plain scalar views.
                    const bool coversWholeValue =
                        range.low == 0 && range.high + 1 == baseValue.width();
                    ValueId frag = ValueId::invalid();
                    if (coversWholeValue && !baseValue.shape().empty())
                    {
                        frag = graph.createValue(static_cast<int32_t>(width),
                                                 baseValue.isSigned(),
                                                 baseValue.type(),
                                                 baseValue.shape());
                    }
                    else
                    {
                        frag = graph.createValue(static_cast<int32_t>(width),
                                                 baseValue.isSigned(),
                                                 baseValue.type());
                    }
                    fragmentMap.emplace(RangeNode{valueId, range}, frag);
                }
            }

            struct SegmentOp
            {
                OperationKind kind;
                std::vector<ValueId> operands;
                ValueId result;
            };

            struct CustomRewrite
            {
                OperationKind kind;
                std::vector<ValueId> operands;
                ValueId result;
                std::vector<BitRange> resultSegments;
                std::optional<SrcLoc> srcLoc;
            };

            std::unordered_map<OperationId, std::vector<std::pair<ValueId, ValueId>>, OperationIdHash> opPairs;
            std::unordered_map<OperationId, std::vector<SegmentOp>, OperationIdHash> segmentOps;
            std::unordered_map<OperationId, CustomRewrite, OperationIdHash> customOps;
            opPairs.reserve(loop.loopOps.size());
            segmentOps.reserve(loop.loopOps.size());
            customOps.reserve(loop.loopOps.size());
            std::unordered_set<ValueId, ValueIdHash> definedFragments;
            opsRewritten = 0;

            // Loop-split synthesis inherits the union of every loop op's
            // origins (def-site semantics; see kOriginModulesKey).
            std::vector<std::string> loopOrigins;
            for (const OperationId loopOp : loop.loopOps)
            {
                if (!loopOp.valid())
                {
                    continue;
                }
                if (const auto origins = getOriginModules(graph, loopOp))
                {
                    appendOriginModules(loopOrigins, *origins);
                }
            }
            auto stampOrigins = [&](OperationId opId)
            {
                if (!loopOrigins.empty())
                {
                    graph.setAttr(opId, kOriginModulesKey, loopOrigins);
                }
            };

            auto getFragment = [&](ValueId valueId, const BitRange &range) -> std::optional<ValueId>
            {
                const BitRange full{0, graph.getValue(valueId).width() - 1};
                if (loopSet.find(valueId) != loopSet.end())
                {
                    auto it = fragmentMap.find(RangeNode{valueId, range});
                    if (it == fragmentMap.end())
                    {
                        const auto segIt = segments.find(valueId);
                        if (segIt == segments.end())
                        {
                            return std::nullopt;
                        }
                        for (const auto &seg : segIt->second)
                        {
                            if (!rangeWithin(range, seg))
                            {
                                continue;
                            }
                            auto segFragIt = fragmentMap.find(RangeNode{valueId, seg});
                            if (segFragIt == fragmentMap.end())
                            {
                                return std::nullopt;
                            }
                            if (range.low == seg.low && range.high == seg.high)
                            {
                                return segFragIt->second;
                            }
                            const int64_t width = range.high - range.low + 1;
                            if (width <= 0)
                            {
                                return std::nullopt;
                            }
                            const int64_t localLow = range.low - seg.low;
                            const int64_t localHigh = range.high - seg.low;
                            OperationId sliceOp = graph.createOperation(OperationKind::kSliceStatic);
                            graph.addOperand(sliceOp, segFragIt->second);
                            graph.setAttr(sliceOp, "sliceStart", localLow);
                            graph.setAttr(sliceOp, "sliceEnd", localHigh);
                            ValueId frag = graph.createValue(static_cast<int32_t>(width),
                                                             graph.getValue(valueId).isSigned(),
                                                             graph.getValue(valueId).type());
                            graph.addResult(sliceOp, frag);
                            stampOrigins(sliceOp);
                            fragmentMap.emplace(RangeNode{valueId, range}, frag);
                            return frag;
                        }
                        return std::nullopt;
                    }
                    return it->second;
                }
                if (range.low == full.low && range.high == full.high)
                {
                    return valueId;
                }
                if (range.low < full.low || range.high > full.high)
                {
                    return std::nullopt;
                }
                auto it = fragmentMap.find(RangeNode{valueId, range});
                if (it != fragmentMap.end())
                {
                    return it->second;
                }
                const int64_t width = range.high - range.low + 1;
                if (width <= 0)
                {
                    return std::nullopt;
                }
                OperationId sliceOp = graph.createOperation(OperationKind::kSliceStatic);
                graph.addOperand(sliceOp, valueId);
                graph.setAttr(sliceOp, "sliceStart", range.low);
                graph.setAttr(sliceOp, "sliceEnd", range.high);
                ValueId frag = graph.createValue(static_cast<int32_t>(width),
                                                 graph.getValue(valueId).isSigned(),
                                                 graph.getValue(valueId).type());
                graph.addResult(sliceOp, frag);
                stampOrigins(sliceOp);
                fragmentMap.emplace(RangeNode{valueId, range}, frag);
                return frag;
            };

            std::unordered_map<ValueId, ValueId, ValueIdHash> fullViewCache;
            auto getFullView = [&](ValueId valueId) -> std::optional<ValueId>
            {
                if (loopSet.find(valueId) == loopSet.end())
                {
                    return valueId;
                }
                auto cached = fullViewCache.find(valueId);
                if (cached != fullViewCache.end())
                {
                    return cached->second;
                }
                const auto segIt = segments.find(valueId);
                if (segIt == segments.end())
                {
                    return std::nullopt;
                }
                std::vector<std::pair<BitRange, ValueId>> ordered;
                ordered.reserve(segIt->second.size());
                for (const auto &seg : segIt->second)
                {
                    auto frag = getFragment(valueId, seg);
                    if (!frag)
                    {
                        return std::nullopt;
                    }
                    ordered.push_back(std::make_pair(seg, *frag));
                }
                std::sort(ordered.begin(), ordered.end(),
                          [](const auto &lhs, const auto &rhs) {
                              if (lhs.first.high != rhs.first.high)
                              {
                                  return lhs.first.high > rhs.first.high;
                              }
                              return lhs.first.low > rhs.first.low;
                          });
                if (ordered.size() == 1)
                {
                    fullViewCache.emplace(valueId, ordered.front().second);
                    return ordered.front().second;
                }
                OperationId concatOp = graph.createOperation(OperationKind::kConcat);
                for (const auto &entry : ordered)
                {
                    graph.addOperand(concatOp, entry.second);
                }
                const Value value = graph.getValue(valueId);
                ValueId fullView = graph.createValue(value.width(), value.isSigned(), value.type(),
                                                     value.shape());
                graph.addResult(concatOp, fullView);
                stampOrigins(concatOp);
                fullViewCache.emplace(valueId, fullView);
                return fullView;
            };

            auto addPair = [&](OperationId opId, ValueId srcFrag, ValueId dstFrag) -> bool
            {
                if (!srcFrag.valid() || !dstFrag.valid())
                {
                    return false;
                }
                if (!definedFragments.insert(dstFrag).second)
                {
                    return false;
                }
                opPairs[opId].push_back(std::make_pair(srcFrag, dstFrag));
                return true;
            };

            auto rewriteOp = [&](OperationId opId,
                                 OperationKind kind,
                                 const std::vector<ValueId> &operands,
                                 ValueId result) -> bool
            {
                const Operation op = graph.getOperation(opId);
                try
                {
                    graph.setOpKind(opId, kind);
                    for (std::size_t i = 0; i < operands.size(); ++i)
                    {
                        if (graph.getOperation(opId).operands().size() > i)
                        {
                            graph.replaceOperand(opId, i, operands[i]);
                        }
                        else
                        {
                            graph.addOperand(opId, operands[i]);
                        }
                    }
                    while (graph.getOperation(opId).operands().size() > operands.size())
                    {
                        graph.eraseOperand(opId, graph.getOperation(opId).operands().size() - 1);
                    }
                    if (graph.getOperation(opId).results().empty())
                    {
                        graph.addResult(opId, result);
                    }
                    else
                    {
                        graph.replaceResult(opId, 0, result);
                    }
                    while (graph.getOperation(opId).results().size() > 1)
                    {
                        graph.eraseResult(opId, graph.getOperation(opId).results().size() - 1);
                    }
                    graph.eraseAttr(opId, "sliceStart");
                    graph.eraseAttr(opId, "sliceEnd");
                    graph.eraseAttr(opId, "sliceWidth");
                    graph.eraseAttr(opId, "rep");
                }
                catch (const std::exception &ex)
                {
                    onError(op, std::string("Failed to rewrite loop op: ") + ex.what());
                    return false;
                }
                ++opsRewritten;
                return true;
            };

            auto createOp = [&](OperationKind kind,
                                const std::vector<ValueId> &operands,
                                ValueId result,
                                const std::optional<SrcLoc> &srcLoc) -> bool
            {
                try
                {
                    OperationId opId = graph.createOperation(kind);
                    for (ValueId operand : operands)
                    {
                        graph.addOperand(opId, operand);
                    }
                    graph.addResult(opId, result);
                    stampOrigins(opId);
                    if (srcLoc)
                    {
                        graph.setOpSrcLoc(opId, *srcLoc);
                    }
                }
                catch (const std::exception &ex)
                {
                    if (!loop.loopOps.empty())
                    {
                        onError(graph.getOperation(loop.loopOps.front()),
                                std::string("Failed to create loop op: ") + ex.what());
                    }
                    return false;
                }
                ++opsRewritten;
                return true;
            };

            for (OperationId opId : loop.loopOps)
            {
                if (!opId.valid())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                const auto &operands = op.operands();
                const auto &results = op.results();
                if (results.size() != 1)
                {
                    return false;
                }
                const ValueId resultId = results[0];
                if (loopSet.find(resultId) == loopSet.end())
                {
                    continue;
                }
                const auto segIt = segments.find(resultId);
                if (segIt == segments.end())
                {
                    return false;
                }
                const auto &resultSegments = segIt->second;
                switch (op.kind())
                {
                case OperationKind::kAssign:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    for (const auto &seg : resultSegments)
                    {
                        auto srcFrag = getFragment(operands[0], seg);
                        auto dstFrag = getFragment(resultId, seg);
                        if (!srcFrag || !dstFrag || !addPair(opId, *srcFrag, *dstFrag))
                        {
                            return false;
                        }
                    }
                    break;
                }
                case OperationKind::kSliceStatic:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    auto startOpt = getIntAttr(op, "sliceStart");
                    auto endOpt = getIntAttr(op, "sliceEnd");
                    if (!startOpt || !endOpt)
                    {
                        return false;
                    }
                    const int64_t sliceLow = *startOpt;
                    const int64_t sliceHigh = *endOpt;
                    for (const auto &seg : resultSegments)
                    {
                        BitRange baseRange{sliceLow + seg.low, sliceLow + seg.high};
                        auto srcFrag = getFragment(operands[0], baseRange);
                        auto dstFrag = getFragment(resultId, seg);
                        if (!srcFrag || !dstFrag || !addPair(opId, *srcFrag, *dstFrag))
                        {
                            return false;
                        }
                    }
                    break;
                }
                case OperationKind::kSliceDynamic:
                {
                    if (operands.size() < 2 || !operands[0].valid() || !operands[1].valid())
                    {
                        return false;
                    }
                    auto widthOpt = getIntAttr(op, "sliceWidth");
                    auto indexOpt = getConstIntValue(graph, operands[1]);
                    if (!widthOpt || !indexOpt)
                    {
                        return false;
                    }
                    const int64_t sliceWidth = *widthOpt;
                    const int64_t sliceLow = *indexOpt;
                    const int64_t sliceHigh = sliceLow + sliceWidth - 1;
                    if (sliceWidth <= 0 || sliceLow < 0 || sliceHigh < sliceLow)
                    {
                        return false;
                    }
                    const int64_t operandWidth = graph.getValue(operands[0]).width();
                    if (sliceHigh >= operandWidth)
                    {
                        return false;
                    }
                    for (const auto &seg : resultSegments)
                    {
                        if (seg.low < 0 || seg.high < seg.low || seg.high >= sliceWidth)
                        {
                            return false;
                        }
                        BitRange baseRange{sliceLow + seg.low, sliceLow + seg.high};
                        auto srcFrag = getFragment(operands[0], baseRange);
                        auto dstFrag = getFragment(resultId, seg);
                        if (!srcFrag || !dstFrag || !addPair(opId, *srcFrag, *dstFrag))
                        {
                            return false;
                        }
                    }
                    break;
                }
                case OperationKind::kSliceArray:
                {
                    // stage-05: constant in-bounds indices behave like a
                    // static slice at the selected element range. A dynamic
                    // index cannot be split per element: the result stays one
                    // whole fragment and the read is re-pointed at the
                    // reassembled full view of the (possibly split) base.
                    auto geometry = getSliceArrayGeometry(graph, op);
                    if (!geometry)
                    {
                        return false;
                    }
                    const ValueId baseId = operands[0];
                    if (geometry->selectable)
                    {
                        const int64_t subVolume = geometry->subVolume;
                        for (const auto &seg : resultSegments)
                        {
                            if (seg.low < 0 || seg.high < seg.low || seg.high >= subVolume)
                            {
                                return false;
                            }
                            BitRange baseRange{geometry->offset + seg.low,
                                               geometry->offset + seg.high};
                            auto srcFrag = getFragment(baseId, baseRange);
                            auto dstFrag = getFragment(resultId, seg);
                            if (!srcFrag || !dstFrag || !addPair(opId, *srcFrag, *dstFrag))
                            {
                                return false;
                            }
                        }
                        break;
                    }
                    if (resultSegments.size() != 1)
                    {
                        return false;
                    }
                    const auto &seg = resultSegments.front();
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (seg.low != 0 || seg.high + 1 != resultWidth)
                    {
                        return false;
                    }
                    std::vector<ValueId> newOperands;
                    newOperands.reserve(operands.size());
                    auto baseView = getFullView(baseId);
                    if (!baseView)
                    {
                        return false;
                    }
                    newOperands.push_back(*baseView);
                    for (std::size_t i = 1; i < operands.size(); ++i)
                    {
                        if (!operands[i].valid())
                        {
                            return false;
                        }
                        auto indexView = getFullView(operands[i]);
                        if (!indexView)
                        {
                            return false;
                        }
                        newOperands.push_back(*indexView);
                    }
                    auto dstFrag = getFragment(resultId, seg);
                    if (!dstFrag)
                    {
                        return false;
                    }
                    if (!definedFragments.insert(*dstFrag).second)
                    {
                        return false;
                    }
                    customOps.emplace(opId, CustomRewrite{op.kind(),
                                                          std::move(newOperands),
                                                          resultId,
                                                          resultSegments,
                                                          op.srcLoc()});
                    break;
                }
                case OperationKind::kConcat:
                case OperationKind::kArrayCreate:
                {
                    if (operands.empty())
                    {
                        return false;
                    }
                    std::vector<int64_t> widths;
                    widths.reserve(operands.size());
                    int64_t totalWidth = 0;
                    for (ValueId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            return false;
                        }
                        const int64_t width = graph.getValue(operand).width();
                        if (width <= 0)
                        {
                            return false;
                        }
                        widths.push_back(width);
                        totalWidth += width;
                    }
                    int64_t cursor = totalWidth;
                    for (std::size_t i = 0; i < operands.size(); ++i)
                    {
                        const std::size_t slot = op.kind() == OperationKind::kArrayCreate ? operands.size() - 1 - i : i;
                        const int64_t width = widths[slot];
                        const int64_t hi = cursor - 1;
                        const int64_t lo = cursor - width;
                        cursor = lo;
                        for (const auto &seg : resultSegments)
                        {
                            if (seg.high < lo || seg.low > hi)
                            {
                                continue;
                            }
                            if (seg.low < lo || seg.high > hi)
                            {
                                return false;
                            }
                            BitRange opRange{seg.low - lo, seg.high - lo};
                            auto srcFrag = getFragment(operands[slot], opRange);
                            auto dstFrag = getFragment(resultId, seg);
                            if (!srcFrag || !dstFrag || !addPair(opId, *srcFrag, *dstFrag))
                            {
                                return false;
                            }
                        }
                    }
                    break;
                }
                case OperationKind::kReplicate:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    auto repOpt = getIntAttr(op, "rep");
                    if (!repOpt || *repOpt <= 0)
                    {
                        return false;
                    }
                    const ValueId operandId = operands[0];
                    const int64_t operandWidth = graph.getValue(operandId).width();
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (operandWidth <= 0 ||
                        operandWidth > std::numeric_limits<int64_t>::max() / *repOpt ||
                        operandWidth * *repOpt != resultWidth)
                    {
                        return false;
                    }
                    for (const auto &seg : resultSegments)
                    {
                        if (seg.low < 0 || seg.high < seg.low || seg.high >= resultWidth)
                        {
                            return false;
                        }
                        const int64_t copyLow = (seg.low / operandWidth) * operandWidth;
                        const int64_t copyHigh = copyLow + operandWidth - 1;
                        if (seg.high > copyHigh)
                        {
                            return false;
                        }
                        BitRange srcRange{seg.low - copyLow, seg.high - copyLow};
                        auto srcFrag = getFragment(operandId, srcRange);
                        auto dstFrag = getFragment(resultId, seg);
                        if (!srcFrag || !dstFrag || !addPair(opId, *srcFrag, *dstFrag))
                        {
                            return false;
                        }
                    }
                    break;
                }
                case OperationKind::kAnd:
                case OperationKind::kOr:
                case OperationKind::kXor:
                case OperationKind::kXnor:
                {
                    if (operands.size() < 2)
                    {
                        return false;
                    }
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (resultWidth <= 0)
                    {
                        return false;
                    }
                    for (ValueId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            return false;
                        }
                        if (graph.getValue(operand).width() != resultWidth)
                        {
                            return false;
                        }
                    }
                    for (const auto &seg : resultSegments)
                    {
                        std::vector<ValueId> segOperands;
                        segOperands.reserve(operands.size());
                        for (ValueId operand : operands)
                        {
                            auto frag = getFragment(operand, seg);
                            if (!frag)
                            {
                                return false;
                            }
                            segOperands.push_back(*frag);
                        }
                        auto dstFrag = getFragment(resultId, seg);
                        if (!dstFrag)
                        {
                            return false;
                        }
                        if (!definedFragments.insert(*dstFrag).second)
                        {
                            return false;
                        }
                        segmentOps[opId].push_back(SegmentOp{op.kind(), std::move(segOperands), *dstFrag});
                    }
                    break;
                }
                case OperationKind::kLogicAnd:
                case OperationKind::kLogicOr:
                {
                    if (operands.size() < 2)
                    {
                        return false;
                    }
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (resultWidth != 1)
                    {
                        return false;
                    }
                    for (ValueId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            return false;
                        }
                        if (graph.getValue(operand).width() != 1)
                        {
                            return false;
                        }
                    }
                    for (const auto &seg : resultSegments)
                    {
                        if (seg.low != 0 || seg.high != 0)
                        {
                            return false;
                        }
                        std::vector<ValueId> segOperands;
                        segOperands.reserve(operands.size());
                        for (ValueId operand : operands)
                        {
                            auto frag = getFragment(operand, seg);
                            if (!frag)
                            {
                                return false;
                            }
                            segOperands.push_back(*frag);
                        }
                        auto dstFrag = getFragment(resultId, seg);
                        if (!dstFrag)
                        {
                            return false;
                        }
                        if (!definedFragments.insert(*dstFrag).second)
                        {
                            return false;
                        }
                        segmentOps[opId].push_back(SegmentOp{op.kind(), std::move(segOperands), *dstFrag});
                    }
                    break;
                }
                case OperationKind::kNot:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (resultWidth <= 0 || graph.getValue(operands[0]).width() != resultWidth)
                    {
                        return false;
                    }
                    for (const auto &seg : resultSegments)
                    {
                        auto srcFrag = getFragment(operands[0], seg);
                        auto dstFrag = getFragment(resultId, seg);
                        if (!srcFrag || !dstFrag)
                        {
                            return false;
                        }
                        if (!definedFragments.insert(*dstFrag).second)
                        {
                            return false;
                        }
                        segmentOps[opId].push_back(SegmentOp{op.kind(), {*srcFrag}, *dstFrag});
                    }
                    break;
                }
                case OperationKind::kLogicNot:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (resultWidth != 1 || graph.getValue(operands[0]).width() != 1)
                    {
                        return false;
                    }
                    for (const auto &seg : resultSegments)
                    {
                        if (seg.low != 0 || seg.high != 0)
                        {
                            return false;
                        }
                        auto srcFrag = getFragment(operands[0], seg);
                        auto dstFrag = getFragment(resultId, seg);
                        if (!srcFrag || !dstFrag)
                        {
                            return false;
                        }
                        if (!definedFragments.insert(*dstFrag).second)
                        {
                            return false;
                        }
                        segmentOps[opId].push_back(SegmentOp{op.kind(), {*srcFrag}, *dstFrag});
                    }
                    break;
                }
                case OperationKind::kReduceAnd:
                case OperationKind::kReduceOr:
                case OperationKind::kReduceXor:
                case OperationKind::kReduceNor:
                case OperationKind::kReduceNand:
                case OperationKind::kReduceXnor:
                {
                    if (operands.size() != 1 || !operands[0].valid())
                    {
                        return false;
                    }
                    const ValueId operandId = operands[0];
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (resultWidth != 1)
                    {
                        return false;
                    }
                    const int64_t operandWidth = graph.getValue(operandId).width();
                    if (operandWidth <= 0)
                    {
                        return false;
                    }
                    auto srcFrag = getFullView(operandId);
                    if (!srcFrag)
                    {
                        return false;
                    }
                    for (const auto &seg : resultSegments)
                    {
                        if (seg.low != 0 || seg.high != 0)
                        {
                            return false;
                        }
                        auto dstFrag = getFragment(resultId, seg);
                        if (!dstFrag)
                        {
                            return false;
                        }
                        if (!definedFragments.insert(*dstFrag).second)
                        {
                            return false;
                        }
                        segmentOps[opId].push_back(SegmentOp{op.kind(), {*srcFrag}, *dstFrag});
                    }
                    break;
                }
                case OperationKind::kMux:
                {
                    if (operands.size() != 3 || !operands[0].valid() || !operands[1].valid() || !operands[2].valid())
                    {
                        return false;
                    }
                    const ValueId condId = operands[0];
                    const ValueId trueId = operands[1];
                    const ValueId falseId = operands[2];
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (resultWidth <= 0)
                    {
                        return false;
                    }
                    if (graph.getValue(condId).width() != 1)
                    {
                        return false;
                    }
                    if (graph.getValue(trueId).width() != resultWidth ||
                        graph.getValue(falseId).width() != resultWidth)
                    {
                        return false;
                    }
                    const BitRange condRange{0, 0};
                    auto condFrag = getFragment(condId, condRange);
                    if (!condFrag)
                    {
                        return false;
                    }
                    for (const auto &seg : resultSegments)
                    {
                        auto trueFrag = getFragment(trueId, seg);
                        auto falseFrag = getFragment(falseId, seg);
                        auto dstFrag = getFragment(resultId, seg);
                        if (!trueFrag || !falseFrag || !dstFrag)
                        {
                            return false;
                        }
                        if (!definedFragments.insert(*dstFrag).second)
                        {
                            return false;
                        }
                        segmentOps[opId].push_back(SegmentOp{op.kind(), {*condFrag, *trueFrag, *falseFrag}, *dstFrag});
                    }
                    break;
                }
                case OperationKind::kAdd:
                {
                    if (operands.size() != 2 || !operands[0].valid() || !operands[1].valid())
                    {
                        return false;
                    }
                    customOps.emplace(opId, CustomRewrite{op.kind(),
                                                          std::vector<ValueId>{operands[0], operands[1]},
                                                          resultId,
                                                          resultSegments,
                                                          op.srcLoc()});
                    for (const auto &seg : resultSegments)
                    {
                        auto dstFrag = getFragment(resultId, seg);
                        if (!dstFrag)
                        {
                            return false;
                        }
                        definedFragments.insert(*dstFrag);
                    }
                    break;
                }
                case OperationKind::kEq:
                case OperationKind::kNe:
                case OperationKind::kCaseEq:
                case OperationKind::kCaseNe:
                case OperationKind::kWildcardEq:
                case OperationKind::kWildcardNe:
                case OperationKind::kLt:
                case OperationKind::kLe:
                case OperationKind::kGe:
                case OperationKind::kGt:
                {
                    if (operands.size() != 2 || !operands[0].valid() || !operands[1].valid())
                    {
                        return false;
                    }
                    const int64_t resultWidth = graph.getValue(resultId).width();
                    if (resultWidth != 1)
                    {
                        return false;
                    }
                    const int64_t lhsWidth = graph.getValue(operands[0]).width();
                    const int64_t rhsWidth = graph.getValue(operands[1]).width();
                    if (lhsWidth <= 0 || rhsWidth <= 0 || lhsWidth != rhsWidth)
                    {
                        return false;
                    }
                    if (resultSegments.size() != 1)
                    {
                        return false;
                    }
                    const auto &seg = resultSegments.front();
                    if (seg.low != 0 || seg.high != 0)
                    {
                        return false;
                    }
                    auto dstFrag = getFragment(resultId, seg);
                    auto lhsView = getFullView(operands[0]);
                    auto rhsView = getFullView(operands[1]);
                    if (!dstFrag || !lhsView || !rhsView)
                    {
                        return false;
                    }
                    if (!definedFragments.insert(*dstFrag).second)
                    {
                        return false;
                    }
                    customOps.emplace(opId, CustomRewrite{op.kind(),
                                                          std::vector<ValueId>{*lhsView, *rhsView},
                                                          resultId,
                                                          resultSegments,
                                                          op.srcLoc()});
                    break;
                }
                case OperationKind::kSub:
                case OperationKind::kMul:
                case OperationKind::kDiv:
                case OperationKind::kMod:
                case OperationKind::kShl:
                case OperationKind::kLShr:
                case OperationKind::kAShr:
                case OperationKind::kRegisterReadArrayPort:
                {
                    // Monolithic rewrite: the result is a single whole-value
                    // fragment (unsplittable, see splitFalseLoop's cut
                    // collection); operands reassemble as full views.
                    if (resultSegments.size() != 1)
                    {
                        return false;
                    }
                    const auto &seg = resultSegments.front();
                    if (seg.low != 0 ||
                        seg.high + 1 != graph.getValue(resultId).width())
                    {
                        return false;
                    }
                    auto dstFrag = getFragment(resultId, seg);
                    if (!dstFrag)
                    {
                        return false;
                    }
                    std::vector<ValueId> views;
                    views.reserve(operands.size());
                    for (ValueId operand : operands)
                    {
                        if (!operand.valid())
                        {
                            return false;
                        }
                        auto view = getFullView(operand);
                        if (!view)
                        {
                            return false;
                        }
                        views.push_back(*view);
                    }
                    if (!definedFragments.insert(*dstFrag).second)
                    {
                        return false;
                    }
                    customOps.emplace(opId, CustomRewrite{op.kind(), std::move(views),
                                                          resultId, resultSegments,
                                                          op.srcLoc()});
                    break;
                }
                default:
                    return false;
                }
            }

            opsRewritten = 0;
            for (const auto &entry : opPairs)
            {
                const OperationId opId = entry.first;
                const auto &pairs = entry.second;
                if (pairs.empty())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                const ValueId firstSrc = pairs.front().first;
                const ValueId firstDst = pairs.front().second;
                try
                {
                    graph.setOpKind(opId, wolvrix::lib::grh::OperationKind::kAssign);
                    if (op.operands().empty())
                    {
                        graph.addOperand(opId, firstSrc);
                    }
                    else
                    {
                        graph.replaceOperand(opId, 0, firstSrc);
                    }
                    while (graph.getOperation(opId).operands().size() > 1)
                    {
                        graph.eraseOperand(opId, graph.getOperation(opId).operands().size() - 1);
                    }

                    if (op.results().empty())
                    {
                        graph.addResult(opId, firstDst);
                    }
                    else
                    {
                        graph.replaceResult(opId, 0, firstDst);
                    }
                    if (graph.getOperation(opId).results().size() > 1)
                    {
                        return false;
                    }
                    graph.eraseAttr(opId, "sliceStart");
                    graph.eraseAttr(opId, "sliceEnd");
                    graph.eraseAttr(opId, "sliceWidth");
                    graph.eraseAttr(opId, "rep");
                }
                catch (const std::exception &ex)
                {
                    onError(op, std::string("Failed to rewrite loop op: ") + ex.what());
                    return false;
                }
                ++opsRewritten;
                for (std::size_t i = 1; i < pairs.size(); ++i)
                {
                    const auto &pair = pairs[i];
                    OperationId newOp = graph.createOperation(wolvrix::lib::grh::OperationKind::kAssign);
                    graph.addOperand(newOp, pair.first);
                    graph.addResult(newOp, pair.second);
                    stampOrigins(newOp);
                    if (op.srcLoc())
                    {
                        graph.setOpSrcLoc(newOp, *op.srcLoc());
                    }
                    ++opsRewritten;
                }
            }

            for (const auto &entry : segmentOps)
            {
                const OperationId opId = entry.first;
                const auto &segments = entry.second;
                if (segments.empty())
                {
                    continue;
                }
                const Operation op = graph.getOperation(opId);
                const auto &first = segments.front();
                try
                {
                    graph.setOpKind(opId, first.kind);
                    for (std::size_t i = 0; i < first.operands.size(); ++i)
                    {
                        if (graph.getOperation(opId).operands().size() > i)
                        {
                            graph.replaceOperand(opId, i, first.operands[i]);
                        }
                        else
                        {
                            graph.addOperand(opId, first.operands[i]);
                        }
                    }
                    while (graph.getOperation(opId).operands().size() > first.operands.size())
                    {
                        graph.eraseOperand(opId, graph.getOperation(opId).operands().size() - 1);
                    }

                    if (op.results().empty())
                    {
                        graph.addResult(opId, first.result);
                    }
                    else
                    {
                        graph.replaceResult(opId, 0, first.result);
                    }
                    while (graph.getOperation(opId).results().size() > 1)
                    {
                        graph.eraseResult(opId, graph.getOperation(opId).results().size() - 1);
                    }
                }
                catch (const std::exception &ex)
                {
                    onError(op, std::string("Failed to rewrite loop op: ") + ex.what());
                    return false;
                }
                ++opsRewritten;
                for (std::size_t i = 1; i < segments.size(); ++i)
                {
                    const auto &seg = segments[i];
                    OperationId newOp = graph.createOperation(seg.kind);
                    for (ValueId operand : seg.operands)
                    {
                        graph.addOperand(newOp, operand);
                    }
                    graph.addResult(newOp, seg.result);
                    stampOrigins(newOp);
                    if (op.srcLoc())
                    {
                        graph.setOpSrcLoc(newOp, *op.srcLoc());
                    }
                    ++opsRewritten;
                }
            }

            for (const auto &entry : customOps)
            {
                const OperationId opId = entry.first;
                const auto &custom = entry.second;
                if (isCompareOp(custom.kind) || custom.kind == OperationKind::kSliceArray ||
                    isMonolithicLoopOp(custom.kind))
                {
                    // kSliceArray (dynamic index), compares, and monolithic
                    // loop ops share the "rewrite in place with reassembled
                    // operands" shape.
                    auto dstFrag = getFragment(custom.result, custom.resultSegments.front());
                    if (!dstFrag)
                    {
                        return false;
                    }
                    if (!rewriteOp(opId, custom.kind, custom.operands, *dstFrag))
                    {
                        return false;
                    }
                    continue;
                }
                if (custom.kind != OperationKind::kAdd)
                {
                    return false;
                }

                const int64_t resultWidth = graph.getValue(custom.result).width();
                if (resultWidth <= 0)
                {
                    return false;
                }

                const Value resultValue = graph.getValue(custom.result);
                std::vector<ValueId> bitResults(static_cast<std::size_t>(resultWidth));
                std::optional<ValueId> carryBit;

                auto emitNewBinary = [&](OperationKind kind, ValueId lhs, ValueId rhs) -> std::optional<ValueId>
                {
                    ValueId out = graph.createValue(1, false, resultValue.type());
                    if (!createOp(kind, std::vector<ValueId>{lhs, rhs}, out, custom.srcLoc))
                    {
                        return std::nullopt;
                    }
                    return out;
                };

                for (int64_t bit = 0; bit < resultWidth; ++bit)
                {
                    BitRange bitRange{bit, bit};
                    auto lhsBit = getFragment(custom.operands[0], bitRange);
                    auto rhsBit = getFragment(custom.operands[1], bitRange);
                    if (!lhsBit || !rhsBit)
                    {
                        return false;
                    }
                    auto xor1 = emitNewBinary(OperationKind::kXor, *lhsBit, *rhsBit);
                    auto and1 = emitNewBinary(OperationKind::kAnd, *lhsBit, *rhsBit);
                    if (!xor1 || !and1)
                    {
                        return false;
                    }
                    ValueId sumBit = *xor1;
                    if (carryBit)
                    {
                        auto xor2 = emitNewBinary(OperationKind::kXor, *xor1, *carryBit);
                        auto and2 = emitNewBinary(OperationKind::kAnd, *xor1, *carryBit);
                        if (!xor2 || !and2)
                        {
                            return false;
                        }
                        auto carryNext = emitNewBinary(OperationKind::kOr, *and1, *and2);
                        if (!carryNext)
                        {
                            return false;
                        }
                        sumBit = *xor2;
                        carryBit = *carryNext;
                    }
                    else
                    {
                        carryBit = *and1;
                    }
                    bitResults[static_cast<std::size_t>(bit)] = sumBit;
                }

                bool reused = false;
                auto orderedSegments = custom.resultSegments;
                std::sort(orderedSegments.begin(), orderedSegments.end(),
                          [](const BitRange &lhs, const BitRange &rhs) {
                              if (lhs.low != rhs.low)
                              {
                                  return lhs.low < rhs.low;
                              }
                              return lhs.high < rhs.high;
                          });
                for (const auto &seg : orderedSegments)
                {
                    auto dstFrag = getFragment(custom.result, seg);
                    if (!dstFrag)
                    {
                        return false;
                    }
                    std::vector<ValueId> segBits;
                    segBits.reserve(static_cast<std::size_t>(seg.high - seg.low + 1));
                    for (int64_t bit = seg.high; bit >= seg.low; --bit)
                    {
                        segBits.push_back(bitResults[static_cast<std::size_t>(bit)]);
                    }
                    if (!reused)
                    {
                        if (segBits.size() == 1)
                        {
                            if (!rewriteOp(opId, OperationKind::kAssign, std::vector<ValueId>{segBits.front()}, *dstFrag))
                            {
                                return false;
                            }
                        }
                        else if (!rewriteOp(opId, OperationKind::kConcat, segBits, *dstFrag))
                        {
                            return false;
                        }
                        reused = true;
                    }
                    else if (segBits.size() == 1)
                    {
                        if (!createOp(OperationKind::kAssign, std::vector<ValueId>{segBits.front()}, *dstFrag, custom.srcLoc))
                        {
                            return false;
                        }
                    }
                    else if (!createOp(OperationKind::kConcat, segBits, *dstFrag, custom.srcLoc))
                    {
                        return false;
                    }
                }
            }

            std::unordered_map<ValueId, ValueId, ValueIdHash> viewMap;
            viewMap.reserve(loop.loopValues.size());
            for (ValueId valueId : loop.loopValues)
            {
                const auto segIt = segments.find(valueId);
                if (segIt == segments.end())
                {
                    return false;
                }
                const Value value = graph.getValue(valueId);
                const auto &ranges = segIt->second;
                std::vector<std::pair<BitRange, ValueId>> ordered;
                ordered.reserve(ranges.size());
                for (const auto &range : ranges)
                {
                    auto fragIt = fragmentMap.find(RangeNode{valueId, range});
                    if (fragIt == fragmentMap.end())
                    {
                        return false;
                    }
                    ordered.push_back(std::make_pair(range, fragIt->second));
                }
                std::sort(ordered.begin(), ordered.end(),
                          [](const auto &lhs, const auto &rhs) {
                              if (lhs.first.high != rhs.first.high)
                              {
                                  return lhs.first.high > rhs.first.high;
                              }
                              return lhs.first.low > rhs.first.low;
                          });
                ValueId viewValue = ValueId::invalid();
                if (ordered.size() == 1)
                {
                    viewValue = ordered.front().second;
                }
                else
                {
                    OperationId concatOp = graph.createOperation(OperationKind::kConcat);
                    for (const auto &entry : ordered)
                    {
                        graph.addOperand(concatOp, entry.second);
                    }
                    // Keep the original array shape on the reassembled view so
                    // downstream shape-aware users (kSliceArray, ports) stay
                    // verifier-legal.
                    viewValue = graph.createValue(value.width(), value.isSigned(), value.type(),
                                                  value.shape());
                    graph.addResult(concatOp, viewValue);
                    stampOrigins(concatOp);
                }
                viewMap.emplace(valueId, viewValue);
            }

            for (ValueId valueId : loop.loopValues)
            {
                const Value value = graph.getValue(valueId);
                const auto viewIt = viewMap.find(valueId);
                if (viewIt == viewMap.end())
                {
                    continue;
                }
                const ValueId viewValue = viewIt->second;
                std::vector<wolvrix::lib::grh::ValueUser> users(value.users().begin(), value.users().end());
                for (const auto &user : users)
                {
                    if (!user.operation.valid())
                    {
                        continue;
                    }
                    if (std::find(loop.loopOps.begin(), loop.loopOps.end(), user.operation) != loop.loopOps.end())
                    {
                        continue;
                    }
                    try
                    {
                        graph.replaceOperand(user.operation, user.operandIndex, viewValue);
                    }
                    catch (const std::exception &ex)
                    {
                        onError(graph.getOperation(user.operation),
                                std::string("Failed to rewire external use: ") + ex.what());
                        return false;
                    }
                }
            }

            const auto inputPorts = std::vector<wolvrix::lib::grh::Port>(graph.inputPorts().begin(),
                                                                         graph.inputPorts().end());
            for (const auto &port : inputPorts)
            {
                auto it = viewMap.find(port.value);
                if (it != viewMap.end())
                {
                    graph.bindInputPort(port.name, it->second);
                }
            }
            const auto outputPorts = std::vector<wolvrix::lib::grh::Port>(graph.outputPorts().begin(),
                                                                          graph.outputPorts().end());
            for (const auto &port : outputPorts)
            {
                auto it = viewMap.find(port.value);
                if (it != viewMap.end())
                {
                    graph.bindOutputPort(port.name, it->second);
                }
            }
            const auto inoutPorts = std::vector<wolvrix::lib::grh::InoutPort>(graph.inoutPorts().begin(),
                                                                              graph.inoutPorts().end());
            for (const auto &port : inoutPorts)
            {
                ValueId in = port.in;
                ValueId out = port.out;
                ValueId oe = port.oe;
                if (auto it = viewMap.find(in); it != viewMap.end())
                {
                    in = it->second;
                }
                if (auto it = viewMap.find(out); it != viewMap.end())
                {
                    out = it->second;
                }
                if (auto it = viewMap.find(oe); it != viewMap.end())
                {
                    oe = it->second;
                }
                if (in != port.in || out != port.out || oe != port.oe)
                {
                    graph.bindInoutPort(port.name, in, out, oe);
                }
            }

            return valuesSplit > 0;
        }

    } // namespace

    CombLoopElimPass::CombLoopElimPass()
        : Pass("comb-loop-elim", "comb-loop-elim",
               "Detect combinational loops and record reports"),
          options_({})
    {
    }

    CombLoopElimPass::CombLoopElimPass(CombLoopElimOptions options)
        : Pass("comb-loop-elim", "comb-loop-elim",
               "Detect combinational loops and record reports"),
          options_(options)
    {
    }

    PassResult CombLoopElimPass::run()
    {
        PassResult result;
        logDebug("begin graphs=" + std::to_string(design().graphs().size()));
        std::size_t totalLoopsDetected = 0;
        std::size_t totalTrueLoops = 0;
        std::size_t totalFalseLoops = 0;
        std::size_t totalFalseLoopsFixed = 0;
        std::size_t totalDynArrayLoops = 0;
        std::size_t totalFragCapped = 0;
        std::vector<CombLoopReport> sessionReports;

        for (const auto &entry : design().graphs())
        {
            Graph &graph = *entry.second;
            const std::size_t initialValueCount = graph.values().size();
            if (options_.maxAnalysisNodes > 0 && initialValueCount > options_.maxAnalysisNodes)
            {
                warning(graph, "comb-loop-elim skipped graph (node limit exceeded)");
                continue;
            }

            auto buildSucc = [&]() {
                std::size_t maxSlot = 0;
                for (const ValueId value : graph.values())
                {
                    maxSlot = std::max<std::size_t>(maxSlot, value.index);
                }
                auto trackEndpoints = [&](std::span<const ValueId> operands, std::span<const ValueId> results) {
                    for (const ValueId operand : operands)
                    {
                        maxSlot = std::max<std::size_t>(maxSlot, operand.index);
                    }
                    for (const ValueId result : results)
                    {
                        maxSlot = std::max<std::size_t>(maxSlot, result.index);
                    }
                };
                for (const auto opId : graph.operations())
                {
                    trackEndpoints(graph.opOperands(opId), graph.opResults(opId));
                }

                DenseSucc succ;
                succ.offsets.assign(maxSlot + 2, 0);
                for (const auto opId : graph.operations())
                {
                    const OperationKind opKind = graph.opKind(opId);
                    if (isBoundaryOp(opKind) &&
                        opKind != OperationKind::kRegisterReadArrayPort)
                    {
                        continue;
                    }

                    const auto operands = graph.opOperands(opId);
                    const auto results = graph.opResults(opId);
                    if (operands.empty() || results.empty())
                    {
                        continue;
                    }

                    for (ValueId operand : operands)
                    {
                        succ.offsets[operand.index + 1] += results.size();
                    }
                }
                for (std::size_t i = 1; i < succ.offsets.size(); ++i)
                {
                    succ.offsets[i] += succ.offsets[i - 1];
                }
                succ.targets.resize(succ.offsets[maxSlot + 1]);
                std::vector<uint64_t> cursor(succ.offsets.begin(), succ.offsets.end() - 1);
                for (const auto opId : graph.operations())
                {
                    const OperationKind opKind = graph.opKind(opId);
                    if (isBoundaryOp(opKind) &&
                        opKind != OperationKind::kRegisterReadArrayPort)
                    {
                        continue;
                    }

                    const auto operands = graph.opOperands(opId);
                    const auto results = graph.opResults(opId);
                    if (operands.empty() || results.empty())
                    {
                        continue;
                    }

                    for (ValueId operand : operands)
                    {
                        uint64_t &cur = cursor[operand.index];
                        for (ValueId resultValue : results)
                        {
                            succ.targets[cur++] = resultValue;
                        }
                    }
                }
                return succ;
            };

            auto classifyLoops = [&](std::vector<LoopInfo> &loops,
                                     std::size_t &falseLoops,
                                     std::size_t &trueLoops,
                                     bool analyzeFalseLoops) {
                falseLoops = 0;
                trueLoops = 0;
                if (!analyzeFalseLoops)
                {
                    for (auto &loop : loops)
                    {
                        loop.isFalseLoop = false;
                        loop.analysisIncomplete = false;
                        loop.hasDynamicArrayRead = false;
                    }
                    trueLoops = loops.size();
                    return;
                }
                for (auto &loop : loops)
                {
                    bool incomplete = false;
                    loop.hasDynamicArrayRead = false;
                    loop.isFalseLoop = classifyFalseLoop(loop, graph, options_, incomplete);
                    loop.analysisIncomplete = incomplete;
                    (void)incomplete;
                    if (loop.isFalseLoop)
                    {
                        ++falseLoops;
                    }
                    else
                    {
                        ++trueLoops;
                    }
                }
            };

            auto emitLoopDiagnostic = [&](PassDiagnosticKind kind,
                                          const LoopInfo &loop,
                                          const std::string &message) {
                for (ValueId valueId : loop.loopValues)
                {
                    const Value value = graph.getValue(valueId);
                    if (value.srcLoc())
                    {
                        if (kind == PassDiagnosticKind::Info)
                        {
                            info(graph, value, message);
                        }
                        else
                        {
                            warning(graph, value, message);
                        }
                        return;
                    }
                }
                for (OperationId opId : loop.loopOps)
                {
                    if (!opId.valid())
                    {
                        continue;
                    }
                    const Operation op = graph.getOperation(opId);
                    if (op.srcLoc())
                    {
                        if (kind == PassDiagnosticKind::Info)
                        {
                            info(graph, op, message);
                        }
                        else
                        {
                            warning(graph, op, message);
                        }
                        return;
                    }
                }
                if (kind == PassDiagnosticKind::Info)
                {
                    info(graph, message);
                }
                else
                {
                    warning(graph, message);
                }
            };

            auto recordWarningLoop = [&](const LoopInfo &loop) {
                CombLoopReport report;
                report.graphName = graph.symbol();
                report.loopValues = loop.loopValues;
                report.loopOps = loop.loopOps;
                report.sourceLocation = findLoopSrcLoc(graph, loop.loopValues, loop.loopOps);
                std::string status = "true";
                if (loop.wasFalseLoopCandidate || loop.isFalseLoop)
                {
                    status = options_.fixFalseLoops ? "false-candidate-unresolved"
                                                    : "false-candidate-fix-disabled";
                }
                report.status = status;
                report.diagnosticKind = "warning";
                std::string analysisNote;
                if (loop.analysisIncomplete)
                {
                    analysisNote = "analysis=incomplete";
                }
                if (loop.hasDynamicArrayRead)
                {
                    if (!analysisNote.empty())
                    {
                        analysisNote.push_back(' ');
                    }
                    analysisNote.append("dynamic-array-read");
                }
                report.description = describeLoop(loop, graph, report.sourceLocation, status, analysisNote);
                if (loop.wasFalseLoopCandidate || loop.isFalseLoop)
                {
                    if (options_.fixFalseLoops)
                    {
                        report.description.append("\n  note=false loop candidate; unresolved");
                    }
                    else
                    {
                        report.description.append("\n  note=false loop candidate; fix disabled");
                    }
                }
                emitLoopDiagnostic(PassDiagnosticKind::Warning, loop, report.description);
                if (!options_.outputKey.empty())
                {
                    sessionReports.push_back(std::move(report));
                }
                if (options_.failOnTrueLoop)
                {
                    result.failed = true;
                }
            };

            std::size_t fixIterations = 0;
            std::size_t totalValuesSplit = 0;
            std::size_t totalOpsRewritten = 0;
            std::size_t totalSliceRetargeted = 0;
            std::size_t totalFragCappedLoops = 0;
            std::size_t falseLoopsFixed = 0;
            std::size_t initialLoopsDetected = 0;
            std::size_t initialTrueLoops = 0;
            std::size_t initialFalseLoops = 0;
            std::size_t initialDynArrayLoops = 0;
            bool graphChanged = false;

            {
                const auto valueSpan = graph.values();
                DenseSucc succ = buildSucc();
                std::vector<LoopInfo> initialLoops = collectLoopInfos(graph, valueSpan, succ);
                if (!initialLoops.empty())
                {
                    std::size_t falseLoops = 0;
                    std::size_t trueLoops = 0;
                    classifyLoops(initialLoops, falseLoops, trueLoops, true);
                    initialLoopsDetected = initialLoops.size();
                    initialTrueLoops = trueLoops;
                    initialFalseLoops = falseLoops;
                    for (const auto &loop : initialLoops)
                    {
                        if (loop.hasDynamicArrayRead)
                        {
                            ++initialDynArrayLoops;
                        }
                    }
                }
            }

            if (options_.fixFalseLoops)
            {
                for (std::size_t iter = 0; iter < options_.maxFixIterations; ++iter)
                {
                    const auto valueSpan = graph.values();
                    DenseSucc succ = buildSucc();
                    std::vector<LoopInfo> loops = collectLoopInfos(graph, valueSpan, succ);
                    if (loops.empty())
                    {
                        break;
                    }

                    std::size_t falseLoops = 0;
                    std::size_t trueLoops = 0;
                    classifyLoops(loops, falseLoops, trueLoops, true);
                    if (falseLoops == 0)
                    {
                        break;
                    }

                    bool changedThisIter = false;
                    for (const auto &loop : loops)
                    {
                        if (!loop.isFalseLoop)
                        {
                            continue;
                        }
                        auto onError = [&](const Operation &op, std::string msg) {
                            error(graph, op, std::move(msg));
                            result.failed = true;
                        };
                        std::size_t loopSliceRetargeted = 0;
                        if (retargetLoopSliceEntrances(graph, loop, loopSliceRetargeted, onError))
                        {
                            changedThisIter = true;
                            totalSliceRetargeted += loopSliceRetargeted;
                            continue;
                        }
                        std::size_t loopValuesSplit = 0;
                        std::size_t loopOpsRewritten = 0;
                        bool hitFragmentCap = false;
                        std::optional<SrcLoc> loopLoc = findLoopSrcLoc(graph, loop.loopValues, loop.loopOps);
                        if (splitFalseLoop(graph, loop, options_, loopValuesSplit, loopOpsRewritten,
                                           hitFragmentCap, onError))
                        {
                            changedThisIter = true;
                            totalValuesSplit += loopValuesSplit;
                            totalOpsRewritten += loopOpsRewritten;
                            std::string detail = "graph=" + graph.symbol();
                            detail.append(" ");
                            detail.append(describeLoop(loop, graph, loopLoc, "false",
                                                       loop.analysisIncomplete ? "analysis=incomplete" : ""));
                            detail.append("\n  action=split");
                            detail.append(" split-values=");
                            detail.append(std::to_string(loopValuesSplit));
                            detail.append(" split-ops=");
                            detail.append(std::to_string(loopOpsRewritten));
                            detail.append(" iter=");
                            detail.append(std::to_string(iter));
                            logDebug(std::move(detail));
                        }
                        else if (hitFragmentCap)
                        {
                            ++totalFragCappedLoops;
                            std::string detail = "comb-loop-elim skipped split (fragment cap exceeded) graph=";
                            detail.append(graph.symbol());
                            detail.append(" max-fragments-per-value=");
                            detail.append(std::to_string(options_.maxFragmentsPerValue));
                            logDebug(std::move(detail));
                        }
                    }

                    if (!changedThisIter)
                    {
                        break;
                    }
                    graphChanged = true;
                    ++fixIterations;
                }
            }

            const auto finalValues = graph.values();
            DenseSucc finalSucc = buildSucc();
            std::vector<LoopInfo> finalLoops = collectLoopInfos(graph, finalValues, finalSucc);
            std::size_t falseLoops = 0;
            std::size_t trueLoops = 0;
            classifyLoops(finalLoops, falseLoops, trueLoops, options_.fixFalseLoops);
            std::size_t unresolvedFalseLoops = 0;
            if (options_.fixFalseLoops && falseLoops > 0)
            {
                for (auto &loop : finalLoops)
                {
                    if (!loop.isFalseLoop)
                    {
                        continue;
                    }
                    loop.wasFalseLoopCandidate = true;
                    loop.isFalseLoop = false;
                    ++unresolvedFalseLoops;
                }
                if (unresolvedFalseLoops > 0)
                {
                    trueLoops += unresolvedFalseLoops;
                    falseLoops -= unresolvedFalseLoops;
                }
            }
            if (options_.fixFalseLoops && initialFalseLoops >= unresolvedFalseLoops)
            {
                falseLoopsFixed = initialFalseLoops - unresolvedFalseLoops;
            }

            std::size_t loopsDetected = initialLoopsDetected;
            for (const auto &loop : finalLoops)
            {
                recordWarningLoop(loop);
            }

            if (falseLoopsFixed > 0)
            {
                std::string message = "comb-loop-elim resolved false loops graph=" + graph.symbol();
                message.append(" fixed=");
                message.append(std::to_string(falseLoopsFixed));
                message.append(" retargeted-slices=");
                message.append(std::to_string(totalSliceRetargeted));
                message.append(" split-values=");
                message.append(std::to_string(totalValuesSplit));
                message.append(" split-ops=");
                message.append(std::to_string(totalOpsRewritten));
                info(graph, message);

                if (!options_.outputKey.empty())
                {
                    CombLoopReport report;
                    report.graphName = graph.symbol();
                    report.status = "false-fixed";
                    report.diagnosticKind = "info";
                    report.description = message;
                    sessionReports.push_back(std::move(report));
                }
            }

            if (graphChanged)
            {
                result.changed = true;
            }

            if (loopsDetected > 0)
            {
                std::string stats = "comb-loop-elim graph=" + graph.symbol();
                stats.append(" loops=");
                stats.append(std::to_string(loopsDetected));
                stats.append(" true=");
                stats.append(std::to_string(initialTrueLoops));
                stats.append(" false=");
                stats.append(std::to_string(initialFalseLoops));
                stats.append(" false-unresolved=");
                stats.append(std::to_string(unresolvedFalseLoops));
                stats.append(" false-fixed=");
                stats.append(std::to_string(falseLoopsFixed));
                stats.append(" retargeted-slices=");
                stats.append(std::to_string(totalSliceRetargeted));
                stats.append(" split-values=");
                stats.append(std::to_string(totalValuesSplit));
                stats.append(" split-ops=");
                stats.append(std::to_string(totalOpsRewritten));
                stats.append(" fix-iters=");
                stats.append(std::to_string(fixIterations));
                stats.append(" dyn-array=");
                stats.append(std::to_string(initialDynArrayLoops));
                stats.append(" frag-capped=");
                stats.append(std::to_string(totalFragCappedLoops));
                logInfo(std::move(stats));
            }

            totalLoopsDetected += loopsDetected;
            totalTrueLoops += initialTrueLoops;
            totalFalseLoops += initialFalseLoops;
            totalFalseLoopsFixed += falseLoopsFixed;
            totalDynArrayLoops += initialDynArrayLoops;
            totalFragCapped += totalFragCappedLoops;
        }

        std::string summary = "comb-loop-elim summary";
        summary.append(" loops=");
        summary.append(std::to_string(totalLoopsDetected));
        summary.append(" true=");
        summary.append(std::to_string(totalTrueLoops));
        summary.append(" false=");
        summary.append(std::to_string(totalFalseLoops));
        summary.append(" false-fixed=");
        summary.append(std::to_string(totalFalseLoopsFixed));
        summary.append(" dyn-array=");
        summary.append(std::to_string(totalDynArrayLoops));
        summary.append(" frag-capped=");
        summary.append(std::to_string(totalFragCapped));
        if (totalLoopsDetected > 0)
        {
            info(summary);
        }
        logInfo(std::move(summary));

        if (!options_.outputKey.empty())
        {
            setSessionValue(options_.outputKey, std::move(sessionReports), "comb-loop.reports");
        }

        return result;
    }

} // namespace wolvrix::lib::transform
