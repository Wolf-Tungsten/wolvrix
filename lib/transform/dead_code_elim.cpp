#include "transform/dead_code_elim.hpp"

#include "core/grh.hpp"
#include "ingest/node_attributes.hpp"
#include "constant_index.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;

        // A liveness region is a set of half-open flat bit intervals.  The
        // DCE root mask still records why an item is live; the region records
        // which part of a value is needed.  Keeping this separate from the
        // root mask lets aggregate/array values shed operands that are only
        // present in an overwritten slice.
        struct LiveRegion
        {
            bool all = false;
            std::vector<std::pair<int64_t, int64_t>> ranges;

            static LiveRegion allBits() { return {.all = true}; }

            bool empty() const noexcept { return !all && ranges.empty(); }

            bool merge(const LiveRegion &other)
            {
                if (all) return false;
                if (other.all) {
                    all = true;
                    ranges.clear();
                    return true;
                }
                const auto before = ranges;
                for (const auto [begin, end] : other.ranges) {
                    if (begin >= end) continue;
                    ranges.push_back({begin, end});
                }
                if (ranges == before) return false;
                std::sort(ranges.begin(), ranges.end());
                std::vector<std::pair<int64_t, int64_t>> merged;
                merged.reserve(ranges.size());
                for (const auto [begin, end] : ranges) {
                    if (merged.empty() || begin > merged.back().second) merged.push_back({begin, end});
                    else merged.back().second = std::max(merged.back().second, end);
                }
                ranges.swap(merged);
                return ranges != before;
            }

            static LiveRegion interval(int64_t begin, int64_t end)
            {
                LiveRegion result;
                if (begin < end) result.ranges.push_back({begin, end});
                return result;
            }
        };

        struct StorageInfo
        {
            OperationId declOp = OperationId::invalid();
            std::vector<OperationId> writePorts;
            bool live = false;
            bool declaredRoot = false;
            uint32_t rootMask = 0;
            LiveRegion region;
        };

        enum RootMaskBit : uint32_t
        {
            kRootPort = 1u << 0,
            kRootDeclaredValue = 1u << 1,
            kRootDeclaredStorage = 1u << 2,
            kRootInstance = 1u << 3,
            kRootBlackbox = 1u << 4,
            kRootSystemFunction = 1u << 5,
            kRootSystemTask = 1u << 6,
            kRootDpicImport = 1u << 7,
            kRootDpicCall = 1u << 8,
            kRootXmrWrite = 1u << 9,
        };

        struct RootMaskDesc
        {
            uint32_t bit = 0;
            const char *name = "";
        };

        constexpr std::array<RootMaskDesc, 10> kRootMaskTable = {{{kRootPort, "port"},
                                                                   {kRootDeclaredValue, "declared_value"},
                                                                   {kRootDeclaredStorage, "declared_storage"},
                                                                   {kRootInstance, "instance"},
                                                                   {kRootBlackbox, "blackbox"},
                                                                   {kRootSystemFunction, "system_function"},
                                                                   {kRootSystemTask, "system_task"},
                                                                   {kRootDpicImport, "dpic_import"},
                                                                   {kRootDpicCall, "dpic_call"},
                                                                   {kRootXmrWrite, "xmr_write"}}};

        bool isObservableBoundaryOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kInstance:
            case OperationKind::kBlackbox:
            case OperationKind::kSystemFunction:
            case OperationKind::kSystemTask:
            case OperationKind::kDpicImport:
            case OperationKind::kDpicCall:
            case OperationKind::kXMRWrite:
                return true;
            default:
                return false;
            }
        }

        uint32_t rootMaskForObservableBoundaryOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kInstance:
                return kRootInstance;
            case OperationKind::kBlackbox:
                return kRootBlackbox;
            case OperationKind::kSystemFunction:
                return kRootSystemFunction;
            case OperationKind::kSystemTask:
                return kRootSystemTask;
            case OperationKind::kDpicImport:
                return kRootDpicImport;
            case OperationKind::kDpicCall:
                return kRootDpicCall;
            case OperationKind::kXMRWrite:
                return kRootXmrWrite;
            default:
                return 0;
            }
        }

        bool isStateDeclOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kRegister:
            case OperationKind::kLatch:
                return true;
            default:
                return false;
            }
        }

        bool isStateReadOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterReadArrayPort:
            case OperationKind::kLatchReadPort:
                return true;
            default:
                return false;
            }
        }

        bool isStateWriteOp(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kRegisterWritePort:
            case OperationKind::kRegisterWriteMaskPort:
            case OperationKind::kRegisterWriteArrayPort:
            case OperationKind::kRegisterWriteArrayMaskPort:
            case OperationKind::kRegisterFillPort:
            case OperationKind::kLatchWritePort:
                return true;
            default:
                return false;
            }
        }

        StorageInfo &ensureStorage(std::unordered_map<std::string, StorageInfo> &map, const std::string &sym)
        {
            auto [it, inserted] = map.try_emplace(sym);
            (void)inserted;
            return it->second;
        }

        std::unordered_map<std::string, StorageInfo> *storageMapForKind(
            OperationKind kind,
            std::unordered_map<std::string, StorageInfo> &regInfos,
            std::unordered_map<std::string, StorageInfo> &latchInfos)
        {
            switch (kind)
            {
            case OperationKind::kRegister:
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterReadArrayPort:
            case OperationKind::kRegisterWritePort:
            case OperationKind::kRegisterWriteMaskPort:
            case OperationKind::kRegisterWriteArrayPort:
            case OperationKind::kRegisterWriteArrayMaskPort:
            case OperationKind::kRegisterFillPort:
                return &regInfos;
            case OperationKind::kLatch:
            case OperationKind::kLatchReadPort:
            case OperationKind::kLatchWritePort:
                return &latchInfos;
            default:
                return nullptr;
            }
        }

        // Screening/storage propagation only needs the operation kind and one
        // symbol attribute.  Borrow those fields directly from the graph so
        // the hot liveness walk does not allocate an Operation snapshot.
        std::optional<std::string> getStorageSymbol(const wolvrix::lib::grh::Graph &graph,
                                                     OperationId opId)
        {
            const OperationKind kind = graph.opKind(opId);
            switch (kind)
            {
            case OperationKind::kRegister:
            case OperationKind::kLatch: {
                const auto symbol = graph.operationSymbol(opId);
                const auto text = graph.symbolText(symbol);
                if (!text.empty())
                {
                    return std::string(text);
                }
                return std::nullopt;
            }
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterReadArrayPort:
            case OperationKind::kRegisterWritePort:
            case OperationKind::kRegisterWriteMaskPort:
            case OperationKind::kRegisterWriteArrayPort:
            case OperationKind::kRegisterWriteArrayMaskPort:
            case OperationKind::kRegisterFillPort:
            case OperationKind::kLatchReadPort:
            case OperationKind::kLatchWritePort: {
                const std::string_view key =
                    (kind == OperationKind::kLatchReadPort || kind == OperationKind::kLatchWritePort)
                        ? "latchSymbol" : "regSymbol";
                for (const auto &attribute : graph.opAttrRange(opId))
                {
                    if (attribute.key() != key)
                    {
                        continue;
                    }
                    if (const auto text = attribute.value().get<std::string>())
                    {
                        return std::string(*text);
                    }
                    return std::nullopt;
                }
                return std::nullopt;
            }
            default:
                return std::nullopt;
            }
        }

        LiveRegion clampRegion(const LiveRegion &region, int64_t width)
        {
            if (width <= 0) return {};
            if (region.all) return LiveRegion::allBits();
            LiveRegion result;
            for (const auto [begin, end] : region.ranges)
                result.merge(LiveRegion::interval(std::max<int64_t>(0, begin), std::min(width, end)));
            if (result.ranges.size() == 1 && result.ranges.front() == std::pair<int64_t, int64_t>{0, width})
                return LiveRegion::allBits();
            return result;
        }

        LiveRegion mapSliceRegion(const LiveRegion &resultRegion, int64_t offset, int64_t width)
        {
            if (resultRegion.all) return LiveRegion::interval(offset, offset + width);
            LiveRegion result;
            for (const auto [begin, end] : resultRegion.ranges) {
                const auto lo = std::max<int64_t>(0, begin);
                const auto hi = std::min(width, end);
                if (lo < hi) result.merge(LiveRegion::interval(offset + lo, offset + hi));
            }
            return result;
        }

        LiveRegion mapPackedOperandRegion(const LiveRegion &resultRegion,
                                          int64_t offset, int64_t width)
        {
            // The operand occupies [offset, offset + width) in the packed
            // result. Intersect demand there, then subtract its low-bit offset
            // to recover operand-local coordinates. Concat computes offsets
            // in reverse operand order; ArrayCreate computes them forwards.
            if (resultRegion.all) return LiveRegion::allBits();
            LiveRegion result;
            for (const auto [begin, end] : resultRegion.ranges) {
                const auto lo = std::max<int64_t>(begin, offset);
                const auto hi = std::min<int64_t>(end, offset + width);
                if (lo < hi) result.merge(LiveRegion::interval(lo - offset, hi - offset));
            }
            return result;
        }

        LiveRegion mapArraySliceRegion(const LiveRegion &resultRegion,
                                       std::span<const ValueId> operands,
                                       const wolvrix::lib::grh::Graph &graph,
                                       int64_t resultWidth)
        {
            if (operands.size() < 2) return {};
            const auto base = operands[0];
            const auto shape = graph.valueShape(base);
            if (shape.empty()) return LiveRegion::allBits();
            const auto indexCount = operands.size() - 1;
            if (indexCount > shape.size()) return LiveRegion::allBits();
            int64_t elementCount = 1;
            int64_t linearIndex = 0;
            for (std::size_t i = 0; i < indexCount; ++i) {
                if (shape[i] <= 0 || elementCount > std::numeric_limits<int64_t>::max() / shape[i])
                    return LiveRegion::allBits();
                const auto index = detail::constantIndex(graph, operands[i + 1]);
                if (!index || *index >= static_cast<uint64_t>(shape[i])) return LiveRegion::allBits();
                linearIndex = linearIndex * shape[i] + static_cast<int64_t>(*index);
                elementCount *= shape[i];
            }
            if (elementCount <= 0 || graph.valueWidth(base) % elementCount != 0)
                return LiveRegion::allBits();
            const auto elementWidth = graph.valueWidth(base) / elementCount;
            return mapSliceRegion(resultRegion, linearIndex * elementWidth,
                                  resultWidth);
        }

        std::string jsonEscape(std::string_view text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (const unsigned char ch : text)
            {
                switch (ch)
                {
                case '\\':
                    out.append("\\\\");
                    break;
                case '"':
                    out.append("\\\"");
                    break;
                case '\b':
                    out.append("\\b");
                    break;
                case '\f':
                    out.append("\\f");
                    break;
                case '\n':
                    out.append("\\n");
                    break;
                case '\r':
                    out.append("\\r");
                    break;
                case '\t':
                    out.append("\\t");
                    break;
                default:
                    if (ch < 0x20)
                    {
                        static constexpr char kHex[] = "0123456789abcdef";
                        out.append("\\u00");
                        out.push_back(kHex[(ch >> 4) & 0xf]);
                        out.push_back(kHex[ch & 0xf]);
                    }
                    else
                    {
                        out.push_back(static_cast<char>(ch));
                    }
                    break;
                }
            }
            return out;
        }

        std::string maskToKey(uint32_t mask)
        {
            if (mask == 0)
            {
                return "none";
            }
            std::string out;
            for (const auto &entry : kRootMaskTable)
            {
                if ((mask & entry.bit) == 0)
                {
                    continue;
                }
                if (!out.empty())
                {
                    out.push_back('|');
                }
                out.append(entry.name);
            }
            return out;
        }

        std::string prefixAtDepth(std::string_view sym, std::size_t depth)
        {
            std::size_t parts = 1;
            for (std::size_t i = 0; i < sym.size(); ++i)
            {
                if (sym[i] != '$')
                {
                    continue;
                }
                if (parts >= depth)
                {
                    return std::string(sym.substr(0, i));
                }
                ++parts;
            }
            return std::string(sym);
        }

    } // namespace

    DeadCodeElimPass::DeadCodeElimPass()
        : Pass("dead-code-elim", "dead-code-elim", "Remove unused operations and values"),
          options_({})
    {
    }

    DeadCodeElimPass::DeadCodeElimPass(DeadCodeElimOptions options)
        : Pass("dead-code-elim", "dead-code-elim", "Remove unused operations and values"),
          options_(std::move(options))
    {
    }

    PassResult DeadCodeElimPass::run()
    {
        const auto timingStart = std::chrono::steady_clock::now();
        stats_ = {};
        PassResult result;
        bool anyChanged = false;
        const std::size_t graphCount = design().graphs().size();
        auto *dirty = getSessionValue<DirtyWorklist>("simplify.dirty");
        const auto activeGraphs = dirty ? dirty->take(id()) : std::nullopt;
        if (dirty && dirty->active && !activeGraphs) return {};
        std::unordered_set<std::string> changedGraphNames;
        logDebug("begin graphs=" + std::to_string(graphCount));
        std::size_t changedGraphs = 0;

        for (const auto &entry : design().graphs())
        {
            if (activeGraphs && !activeGraphs->contains(entry.first)) continue;
            wolvrix::lib::grh::Graph &graph = *entry.second;
            bool graphChanged = false;
            const auto graphTimingStart = std::chrono::steady_clock::now();

            const auto opSpan = graph.operations();
            const auto valueSpan = graph.values();
            if (opSpan.empty() && valueSpan.empty())
            {
                continue;
            }

            uint32_t maxValueIndex = 0;
            for (const auto valueId : valueSpan)
            {
                if (valueId.valid())
                {
                    maxValueIndex = std::max(maxValueIndex, valueId.index);
                }
            }
            uint32_t maxOpIndex = 0;
            for (const auto opId : opSpan)
            {
                if (opId.valid())
                {
                    maxOpIndex = std::max(maxOpIndex, opId.index);
                }
            }

            std::vector<uint8_t> isPort;
            std::vector<uint8_t> isDeclaredValue;
            std::vector<uint32_t> liveValueMask;
            std::vector<uint32_t> liveOpMask;
            std::vector<LiveRegion> liveValueRegion;
            std::vector<LiveRegion> liveOpRegion;
            std::vector<OperationId> defOpByValue;
            if (maxValueIndex > 0)
            {
                isPort.assign(static_cast<std::size_t>(maxValueIndex + 1), 0);
                isDeclaredValue.assign(static_cast<std::size_t>(maxValueIndex + 1), 0);
                liveValueMask.assign(static_cast<std::size_t>(maxValueIndex + 1), 0);
                liveValueRegion.resize(static_cast<std::size_t>(maxValueIndex + 1));
                defOpByValue.assign(static_cast<std::size_t>(maxValueIndex + 1), OperationId::invalid());
            }
            if (maxOpIndex > 0)
            {
                liveOpMask.assign(static_cast<std::size_t>(maxOpIndex + 1), 0);
                liveOpRegion.resize(static_cast<std::size_t>(maxOpIndex + 1));
            }

            auto markPort = [&](ValueId valueId) {
                if (!valueId.valid())
                {
                    return;
                }
                if (valueId.index >= isPort.size())
                {
                    return;
                }
                isPort[valueId.index] = 1;
            };
            for (const auto &port : graph.inputPorts())
            {
                markPort(port.value);
            }
            for (const auto &port : graph.outputPorts())
            {
                markPort(port.value);
            }
            for (const auto &port : graph.inoutPorts())
            {
                markPort(port.in);
                markPort(port.out);
                markPort(port.oe);
            }

            struct OpInfo
            {
                OperationId id;
                OperationKind kind = OperationKind::kConstant;
                std::span<const ValueId> operands;
                std::span<const ValueId> results;
            };

            std::vector<OpInfo> ops;
            ops.reserve(opSpan.size());
            std::vector<int32_t> opIndexById;
            if (maxOpIndex > 0)
            {
                opIndexById.assign(static_cast<std::size_t>(maxOpIndex + 1), -1);
            }

            for (const auto opId : opSpan)
            {
                if (!opId.valid())
                {
                    continue;
                }
                OpInfo info;
                info.id = opId;
                info.kind = graph.opKind(opId);
                info.operands = graph.opOperands(opId);
                info.results = graph.opResults(opId);
                const std::size_t infoIndex = ops.size();
                ops.push_back(std::move(info));
                ++stats_.operationsVisited;
                if (opId.index < opIndexById.size())
                {
                    opIndexById[opId.index] = static_cast<int32_t>(infoIndex);
                }
                for (const auto valueId : ops.back().results)
                {
                    if (valueId.valid() && valueId.index < defOpByValue.size())
                    {
                        defOpByValue[valueId.index] = opId;
                    }
                }
            }

            std::unordered_map<std::string, StorageInfo> regInfos;
            std::unordered_map<std::string, StorageInfo> latchInfos;
            regInfos.reserve(1024);
            latchInfos.reserve(128);

            for (const auto &info : ops)
            {
                switch (info.kind)
                {
                case OperationKind::kRegister:
                case OperationKind::kLatch: {
                    auto sym = getStorageSymbol(graph, info.id);
                    if (sym)
                    {
                        ensureStorage(*storageMapForKind(info.kind, regInfos, latchInfos), *sym).declOp = info.id;
                    }
                    break;
                }
                case OperationKind::kRegisterReadPort:
                case OperationKind::kRegisterReadArrayPort:
                case OperationKind::kLatchReadPort:
                    break;
                case OperationKind::kRegisterWritePort:
                case OperationKind::kRegisterWriteMaskPort:
                case OperationKind::kRegisterWriteArrayPort:
                case OperationKind::kRegisterWriteArrayMaskPort:
                case OperationKind::kRegisterFillPort:
                case OperationKind::kLatchWritePort: {
                    auto sym = getStorageSymbol(graph, info.id);
                    auto *map = storageMapForKind(info.kind, regInfos, latchInfos);
                    if (sym && map)
                    {
                        ensureStorage(*map, *sym).writePorts.push_back(info.id);
                    }
                    break;
                }
                default:
                    break;
                }
            }

            struct ValueWorkItem
            {
                ValueId id = ValueId::invalid();
                uint32_t deltaMask = 0;
                LiveRegion region;
            };
            struct OpWorkItem
            {
                OperationId id = OperationId::invalid();
                uint32_t deltaMask = 0;
                LiveRegion region;
            };

            const auto fullRegion = [&](ValueId value) {
                // Most dependencies demand the whole value. Keep that common
                // case inline instead of allocating one interval per edge.
                return value.valid() && graph.valueWidth(value) > 0 ? LiveRegion::allBits() : LiveRegion{};
            };
            const auto normalizeRegion = [&](ValueId value, const LiveRegion &region) {
                return clampRegion(region, graph.valueWidth(value));
            };
            auto markValueLive = [&](ValueId valueId, uint32_t mask, const LiveRegion &region,
                                     std::deque<ValueWorkItem> &worklist) {
                if (!valueId.valid() || valueId.index >= liveValueMask.size())
                {
                    return;
                }
                const auto normalized = normalizeRegion(valueId, region);
                // An empty demand on a positive-width value carries no data
                // dependency. Preserve the historical structural treatment of
                // zero-width roots until their IR contract changes.
                if (normalized.empty() && graph.valueWidth(valueId) > 0) return;
                const uint32_t prevMask = liveValueMask[valueId.index];
                const uint32_t deltaMask = mask & ~prevMask;
                const bool regionChanged = liveValueRegion[valueId.index].merge(normalized);
                if (deltaMask == 0 && !regionChanged) return;
                liveValueMask[valueId.index] = prevMask | deltaMask;
                // New bits can reach operands not touched by the previous
                // region. They inherit every reason already reaching this
                // value even when no new root-mask bit arrived this time.
                const uint32_t propagationMask = regionChanged ? liveValueMask[valueId.index] : deltaMask;
                worklist.push_back({valueId, propagationMask, normalized});
            };
            auto markOpLive = [&](OperationId opId, uint32_t mask, const LiveRegion &region,
                                  std::deque<OpWorkItem> &worklist) {
                if (!opId.valid() || opId.index >= liveOpMask.size())
                {
                    return;
                }
                const uint32_t prevMask = liveOpMask[opId.index];
                const uint32_t deltaMask = mask & ~prevMask;
                const bool regionChanged = liveOpRegion[opId.index].merge(region);
                if (deltaMask == 0 && !regionChanged) return;
                liveOpMask[opId.index] = prevMask | deltaMask;
                const uint32_t propagationMask = regionChanged ? liveOpMask[opId.index] : deltaMask;
                worklist.push_back({opId, propagationMask, region});
            };
            auto markStorageLive = [&](std::unordered_map<std::string, StorageInfo> &map,
                                       const std::string &sym,
                                       uint32_t mask,
                                       const LiveRegion &region,
                                       std::deque<OpWorkItem> &worklist) {
                auto it = map.find(sym);
                if (it == map.end())
                {
                    return;
                }
                it->second.live = true;
                const uint32_t deltaMask = mask & ~it->second.rootMask;
                const bool regionChanged = it->second.region.merge(region);
                if (deltaMask == 0 && !regionChanged) return;
                it->second.rootMask |= deltaMask;
                const uint32_t propagationMask = regionChanged ? it->second.rootMask : deltaMask;
                if (it->second.declOp.valid())
                {
                    markOpLive(it->second.declOp, propagationMask, region, worklist);
                }
                for (const auto writeOp : it->second.writePorts)
                {
                    markOpLive(writeOp, propagationMask, region, worklist);
                }
            };

            if (keepDeclaredSymbols())
            {
                for (const auto sym : graph.declaredSymbols())
                {
                    const ValueId valueId = graph.findValue(sym);
                    if (valueId.valid() && valueId.index < isDeclaredValue.size())
                    {
                        isDeclaredValue[valueId.index] = 1;
                    }
                    const OperationId opId = graph.findOperation(sym);
                    if (!opId.valid())
                    {
                        continue;
                    }
                    const OperationKind kind = graph.opKind(opId);
                    auto symText = getStorageSymbol(graph, opId);
                    auto *map = storageMapForKind(kind, regInfos, latchInfos);
                    if (symText && map)
                    {
                        ensureStorage(*map, *symText).declaredRoot = true;
                    }
                }
            }

            std::deque<ValueWorkItem> liveValueWorklist;
            std::deque<OpWorkItem> liveOpWorklist;

            for (const auto valueId : valueSpan)
            {
                if (!valueId.valid() || valueId.index >= isPort.size())
                {
                    continue;
                }
                if (isPort[valueId.index] != 0)
                {
                    markValueLive(valueId, kRootPort, fullRegion(valueId), liveValueWorklist);
                }
                if (valueId.index < isDeclaredValue.size() && isDeclaredValue[valueId.index] != 0)
                {
                    markValueLive(valueId, kRootDeclaredValue, fullRegion(valueId), liveValueWorklist);
                }
            }

            for (auto &[sym, info] : regInfos)
            {
                if (info.declaredRoot)
                {
                    markStorageLive(regInfos, sym, kRootDeclaredStorage, LiveRegion::allBits(), liveOpWorklist);
                }
            }
            for (auto &[sym, info] : latchInfos)
            {
                if (info.declaredRoot)
                {
                    markStorageLive(latchInfos, sym, kRootDeclaredStorage, LiveRegion::allBits(), liveOpWorklist);
                }
            }

            for (const auto &info : ops)
            {
                if (isObservableBoundaryOp(info.kind))
                {
                    const uint32_t rootMask = rootMaskForObservableBoundaryOp(info.kind);
                    if (rootMask != 0)
                    {
                        markOpLive(info.id, rootMask, LiveRegion::allBits(), liveOpWorklist);
                    }
                }
            }

            while (!liveValueWorklist.empty() || !liveOpWorklist.empty())
            {
                while (!liveValueWorklist.empty())
                {
                    const ValueWorkItem item = std::move(liveValueWorklist.front());
                    liveValueWorklist.pop_front();
                    const ValueId valueId = item.id;
                    if (!valueId.valid() || valueId.index >= defOpByValue.size())
                    {
                        continue;
                    }
                    const OperationId defOp = defOpByValue[valueId.index];
                    if (defOp.valid())
                    {
                        markOpLive(defOp, item.deltaMask, item.region, liveOpWorklist);
                    }
                }

                while (!liveOpWorklist.empty())
                {
                    const OpWorkItem item = std::move(liveOpWorklist.front());
                    liveOpWorklist.pop_front();
                    const OperationId opId = item.id;
                    if (!opId.valid() || opId.index >= opIndexById.size())
                    {
                        continue;
                    }
                    const int32_t opIdx = opIndexById[opId.index];
                    if (opIdx < 0 || static_cast<std::size_t>(opIdx) >= ops.size())
                    {
                        continue;
                    }

                    const OpInfo &info = ops[static_cast<std::size_t>(opIdx)];
                    if (isStateReadOp(info.kind) || isStateWriteOp(info.kind) || isStateDeclOp(info.kind))
                    {
                        auto sym = getStorageSymbol(graph, opId);
                        auto *map = storageMapForKind(info.kind, regInfos, latchInfos);
                        if (sym && map)
                        {
                            // Indexed-read demand is local to the selected
                            // result, not the whole storage. Until indices are
                            // mapped explicitly, retain the entire family.
                            if (info.kind == OperationKind::kRegisterReadArrayPort)
                                markStorageLive(*map, *sym, item.deltaMask, LiveRegion::allBits(), liveOpWorklist);
                            else
                                markStorageLive(*map, *sym, item.deltaMask, item.region, liveOpWorklist);
                        }
                    }

                    const auto markOperand = [&](std::size_t index, const LiveRegion &region) {
                        if (index < info.operands.size())
                            markValueLive(info.operands[index], item.deltaMask, region, liveValueWorklist);
                    };
                    const auto markAllOperands = [&]() {
                        for (std::size_t i = 0; i < info.operands.size(); ++i)
                            markOperand(i, fullRegion(info.operands[i]));
                    };

                    // Storage demand uses whole-state coordinates; indexed
                    // write/fill data uses coordinates local to one row.
                    // Keep data and controls full-live until the indexed or
                    // broadcast inverse map is explicit.
                    if (info.kind == OperationKind::kRegisterWriteArrayPort ||
                        info.kind == OperationKind::kRegisterWriteArrayMaskPort ||
                        info.kind == OperationKind::kRegisterFillPort) {
                        markAllOperands();
                        continue;
                    }

                    // State writes have a storage-family dependency, but the
                    // data operand only needs the region demanded by the
                    // corresponding read.  Enables, indices and clocks remain
                    // control dependencies and are conservatively full-live.
                    if (isStateWriteOp(info.kind) && !info.operands.empty()) {
                        constexpr std::size_t dataIndex = 1;
                        for (std::size_t i = 0; i < info.operands.size(); ++i)
                            markOperand(i, i == dataIndex ? item.region : fullRegion(info.operands[i]));
                        continue;
                    }

                    switch (info.kind) {
                    case OperationKind::kAssign:
                        markOperand(0, item.region);
                        break;
                    case OperationKind::kConcat: {
                        if (item.region.all) {
                            markAllOperands();
                            break;
                        }
                        int64_t offset = 0;
                        for (std::size_t i = info.operands.size(); i-- > 0;) {
                            const auto width = graph.valueWidth(info.operands[i]);
                            markOperand(i, mapPackedOperandRegion(item.region, offset, width));
                            offset += width;
                        }
                        break;
                    }
                    case OperationKind::kArrayCreate: {
                        int64_t offset = 0;
                        for (std::size_t i = 0; i < info.operands.size(); ++i) {
                            const auto width = graph.valueWidth(info.operands[i]);
                            // ArrayCreate places element i at flat bit offset
                            // i*W; demand maps back by subtracting that offset.
                            markOperand(i, mapPackedOperandRegion(item.region, offset, width));
                            offset += width;
                        }
                        break;
                    }
                    case OperationKind::kSliceStatic: {
                        int64_t start = 0;
                        if (const auto attr = graph.opAttrRange(opId).find("sliceStart"))
                            start = attr->value().get<int64_t>().value_or(0);
                        const auto resultWidth = info.results.empty() ? graph.valueWidth(info.operands[0]) :
                                                  graph.valueWidth(info.results[0]);
                        markOperand(0, mapSliceRegion(item.region, start, resultWidth));
                        break;
                    }
                    case OperationKind::kSliceArray: {
                        const auto resultWidth = info.results.empty() ? graph.valueWidth(info.operands[0]) :
                                                  graph.valueWidth(info.results[0]);
                        markOperand(0, mapArraySliceRegion(item.region, info.operands, graph, resultWidth));
                        for (std::size_t i = 1; i < info.operands.size(); ++i)
                            markOperand(i, fullRegion(info.operands[i]));
                        break;
                    }
                    case OperationKind::kMux:
                        markOperand(0, fullRegion(info.operands[0]));
                        markOperand(1, item.region);
                        markOperand(2, item.region);
                        break;
                    default:
                        markAllOperands();
                        break;
                    }
                }
            }

            if (!options_.outputKey.empty())
            {
                auto recordSample = [&](std::vector<std::string> &samples, const std::string &sym) {
                    if (samples.size() >= options_.sampleLimit)
                    {
                        return;
                    }
                    samples.push_back(sym);
                };
                auto emitPrefixCounts = [&](std::ostringstream &out, const std::map<std::string, std::size_t> &counts, std::size_t limit) {
                    out << "[";
                    std::vector<std::pair<std::string, std::size_t>> items(counts.begin(), counts.end());
                    std::sort(items.begin(), items.end(), [](const auto &lhs, const auto &rhs) {
                        if (lhs.second != rhs.second)
                        {
                            return lhs.second > rhs.second;
                        }
                        return lhs.first < rhs.first;
                    });
                    for (std::size_t i = 0; i < items.size() && i < limit; ++i)
                    {
                        if (i != 0)
                        {
                            out << ",";
                        }
                        out << "{\"prefix\":\"" << jsonEscape(items[i].first) << "\",\"count\":" << items[i].second << "}";
                    }
                    out << "]";
                };
                auto emitSamples = [&](std::ostringstream &out, const std::vector<std::string> &samples) {
                    out << "[";
                    for (std::size_t i = 0; i < samples.size(); ++i)
                    {
                        if (i != 0)
                        {
                            out << ",";
                        }
                        out << "\"" << jsonEscape(samples[i]) << "\"";
                    }
                    out << "]";
                };
                auto emitMaskCounts = [&](std::ostringstream &out, const std::map<uint32_t, std::size_t> &counts) {
                    out << "{";
                    bool first = true;
                    for (const auto &[mask, count] : counts)
                    {
                        if (!first)
                        {
                            out << ",";
                        }
                        first = false;
                        out << "\"" << jsonEscape(maskToKey(mask)) << "\":" << count;
                    }
                    out << "}";
                };

                std::size_t liveRegisters = 0;
                std::size_t deadRegisters = 0;
                std::size_t liveRegistersWithPort = 0;
                std::size_t liveRegistersWithBoundary = 0;
                std::size_t liveRegistersWithDeclared = 0;
                std::size_t liveRegistersPortOnly = 0;
                std::size_t liveRegistersBoundaryOnly = 0;
                std::size_t liveRegistersPortAndBoundary = 0;
                std::map<std::string, std::size_t> livePrefixCounts;
                std::map<std::string, std::size_t> deadPrefixCounts;
                std::map<std::string, std::size_t> boundaryOnlyPrefixCounts;
                std::map<uint32_t, std::size_t> liveRegisterMaskCounts;
                std::map<std::string, std::size_t> liveRegisterRootBitCounts;
                std::vector<std::string> liveSamples;
                std::vector<std::string> deadSamples;
                std::vector<std::string> boundaryOnlySamples;

                constexpr uint32_t kBoundaryMask = kRootInstance | kRootBlackbox | kRootSystemFunction |
                                                   kRootSystemTask | kRootDpicImport | kRootDpicCall | kRootXmrWrite;
                constexpr std::size_t kPrefixDepth = 5;
                constexpr std::size_t kPrefixLimit = 20;

                for (const auto &[sym, info] : regInfos)
                {
                    const std::string prefix = prefixAtDepth(sym, kPrefixDepth);
                    if (info.live)
                    {
                        ++liveRegisters;
                        ++liveRegisterMaskCounts[info.rootMask];
                        livePrefixCounts[prefix] += 1;
                        recordSample(liveSamples, sym);

                        const bool hasPort = (info.rootMask & kRootPort) != 0;
                        const bool hasBoundary = (info.rootMask & kBoundaryMask) != 0;
                        const bool hasDeclared = (info.rootMask & (kRootDeclaredValue | kRootDeclaredStorage)) != 0;
                        if (hasPort)
                        {
                            ++liveRegistersWithPort;
                        }
                        if (hasBoundary)
                        {
                            ++liveRegistersWithBoundary;
                        }
                        if (hasDeclared)
                        {
                            ++liveRegistersWithDeclared;
                        }
                        if (hasPort && !hasBoundary)
                        {
                            ++liveRegistersPortOnly;
                        }
                        if (hasBoundary && !hasPort)
                        {
                            ++liveRegistersBoundaryOnly;
                            boundaryOnlyPrefixCounts[prefix] += 1;
                            recordSample(boundaryOnlySamples, sym);
                        }
                        if (hasBoundary && hasPort)
                        {
                            ++liveRegistersPortAndBoundary;
                        }
                        for (const auto &desc : kRootMaskTable)
                        {
                            if ((info.rootMask & desc.bit) != 0)
                            {
                                liveRegisterRootBitCounts[std::string(desc.name)] += 1;
                            }
                        }
                    }
                    else
                    {
                        ++deadRegisters;
                        deadPrefixCounts[prefix] += 1;
                        recordSample(deadSamples, sym);
                    }
                }

                std::ostringstream report;
                report << "{";
                report << "\"graph\":\"" << jsonEscape(graph.symbol()) << "\",";
                report << "\"keepDeclaredSymbols\":" << (keepDeclaredSymbols() ? "true" : "false") << ",";
                report << "\"registers\":{";
                report << "\"total\":" << regInfos.size() << ",";
                report << "\"live\":" << liveRegisters << ",";
                report << "\"dead\":" << deadRegisters << ",";
                report << "\"liveWithPort\":" << liveRegistersWithPort << ",";
                report << "\"liveWithBoundary\":" << liveRegistersWithBoundary << ",";
                report << "\"liveWithDeclared\":" << liveRegistersWithDeclared << ",";
                report << "\"livePortOnly\":" << liveRegistersPortOnly << ",";
                report << "\"liveBoundaryOnly\":" << liveRegistersBoundaryOnly << ",";
                report << "\"livePortAndBoundary\":" << liveRegistersPortAndBoundary << ",";
                report << "\"liveRootBitCounts\":{";
                bool firstRootBit = true;
                for (const auto &[name, count] : liveRegisterRootBitCounts)
                {
                    if (!firstRootBit)
                    {
                        report << ",";
                    }
                    firstRootBit = false;
                    report << "\"" << jsonEscape(name) << "\":" << count;
                }
                report << "},";
                report << "\"liveMaskCounts\":";
                emitMaskCounts(report, liveRegisterMaskCounts);
                report << ",";
                report << "\"topLivePrefixes\":";
                emitPrefixCounts(report, livePrefixCounts, kPrefixLimit);
                report << ",";
                report << "\"topDeadPrefixes\":";
                emitPrefixCounts(report, deadPrefixCounts, kPrefixLimit);
                report << ",";
                report << "\"topBoundaryOnlyPrefixes\":";
                emitPrefixCounts(report, boundaryOnlyPrefixCounts, kPrefixLimit);
                report << ",";
                report << "\"liveSamples\":";
                emitSamples(report, liveSamples);
                report << ",";
                report << "\"deadSamples\":";
                emitSamples(report, deadSamples);
                report << ",";
                report << "\"boundaryOnlySamples\":";
                emitSamples(report, boundaryOnlySamples);
                report << "}";
                report << "}";
                setSessionValue(options_.outputKey, report.str(), "stats");
            }

            // A frozen graph's value span belongs to its view.  The first
            // erase thaws the graph and releases that view, so retain the ID
            // traversal before beginning mutations.
            const auto removalTimingStart = std::chrono::steady_clock::now();
            const auto analysisMs = static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                removalTimingStart - graphTimingStart).count());
            stats_.analysisMs += analysisMs;
            const std::vector<ValueId> valueOrder(valueSpan.begin(), valueSpan.end());
            ingest::node::DeadRootBatch deadRoots(graph);
            for (const auto &info : ops)
            {
                if (info.id.valid() && info.id.index < liveOpMask.size() && liveOpMask[info.id.index] != 0)
                {
                    continue;
                }
                deadRoots.record(info.id);
                if (graph.eraseOpUnchecked(info.id))
                {
                    graphChanged = true;
                    ++stats_.opsRemoved;
                }
            }
            deadRoots.flush();

            std::vector<ValueId> deadValues;
            for (const auto valueId : valueOrder)
            {
                if (!valueId.valid())
                {
                    continue;
                }
                if (graph.valueIsInput(valueId) || graph.valueIsOutput(valueId) ||
                    graph.valueIsInout(valueId))
                {
                    continue;
                }
                if (valueId.index < liveValueMask.size() && liveValueMask[valueId.index] != 0)
                {
                    continue;
                }
                if (keepDeclaredSymbols() && valueId.index < isDeclaredValue.size() &&
                    isDeclaredValue[valueId.index] != 0)
                {
                    continue;
                }
                if (!graph.valueUsers(valueId).empty())
                {
                    continue;
                }
                const OperationId defOp = graph.valueDef(valueId);
                if (defOp.valid() && defOp.index < liveOpMask.size() && liveOpMask[defOp.index] != 0)
                {
                    continue;
                }
                deadValues.push_back(valueId);
            }
            for (const auto valueId : deadValues)
            {
                if (graph.eraseValueUnchecked(valueId))
                {
                    graphChanged = true;
                    ++stats_.valuesRemoved;
                }
            }

            const auto semanticTimingStart = std::chrono::steady_clock::now();
            const auto removalMs = static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                semanticTimingStart - removalTimingStart).count());
            stats_.removalMs += removalMs;
            const auto semanticStats = graph.pruneSemanticTree();
            const auto semanticPruneMs = static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - semanticTimingStart).count());
            stats_.semanticPruneMs += semanticPruneMs;
            stats_.semanticNodesRemoved += semanticStats.removedNodes;
            stats_.semanticNameBytesRemoved += semanticStats.removedNameBytes;
            stats_.semanticNodesRetained += semanticStats.retainedNodes;
            stats_.semanticIndirectNodesRetained += semanticStats.retainedIndirectNodes;
            stats_.semanticIds += semanticStats.semanticIds;
            const bool semanticChanged = semanticStats.removedNodes != 0;
            if (!options_.outputKey.empty())
            {
                std::ostringstream phase;
                phase << "{\"graph\":\"" << jsonEscape(graph.symbol()) << "\",";
                phase << "\"analysisMs\":" << analysisMs << ",\"removalMs\":" << removalMs
                      << ",\"semanticPruneMs\":" << semanticPruneMs << ",";
                phase << "\"semantic\":{\"nodesRemoved\":" << semanticStats.removedNodes
                      << ",\"nameBytesRemoved\":" << semanticStats.removedNameBytes
                      << ",\"nodesRetained\":" << semanticStats.retainedNodes
                      << ",\"indirectNodesRetained\":" << semanticStats.retainedIndirectNodes
                      << ",\"ids\":" << semanticStats.semanticIds << "}}";
                setSessionValue(options_.outputKey + ".phase", phase.str(), "stats");
            }

            anyChanged = anyChanged || graphChanged || semanticChanged;
            if (graphChanged || semanticChanged) ++changedGraphs;
            if (graphChanged) changedGraphNames.insert(entry.first);
        }

        result.changed = anyChanged;
        result.failed = false;
        if (dirty) {
            for (const auto& graph : changedGraphNames)
                dirty->enqueue("redundant-elim", graph);
        }
        stats_.graphs = graphCount;
        stats_.changedGraphs = changedGraphs;
        std::string message = "graphs=" + std::to_string(graphCount);
        message.append(", changedGraphs=");
        message.append(std::to_string(changedGraphs));
        message.append(result.changed ? ", changed=true" : ", changed=false");
        logDebug(std::move(message));
        if (std::getenv("WOLVRIX_SIMPLIFY_PHASE_TIMING") != nullptr)
        {
            std::fprintf(stderr, "[simplify-phase] pass=dead-code-elim %lldms\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    std::chrono::steady_clock::now() - timingStart)
                                                    .count()));
            std::fprintf(stderr,
                         "[dce-phase] analysis_ms=%zu removal_ms=%zu semantic_prune_ms=%zu "
                         "semantic_nodes_removed=%zu semantic_name_bytes_removed=%zu semantic_ids=%zu\n",
                         stats_.analysisMs, stats_.removalMs, stats_.semanticPruneMs,
                         stats_.semanticNodesRemoved, stats_.semanticNameBytesRemoved, stats_.semanticIds);
        }
        return result;
    }

} // namespace wolvrix::lib::transform
