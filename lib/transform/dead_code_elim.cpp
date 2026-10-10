#include "transform/dead_code_elim.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using wolvrix::lib::grh::Operation;
        using wolvrix::lib::grh::OperationId;
        using wolvrix::lib::grh::OperationIdHash;
        using wolvrix::lib::grh::OperationKind;
        using wolvrix::lib::grh::Value;
        using wolvrix::lib::grh::ValueId;
        using wolvrix::lib::grh::ValueIdHash;

        struct StorageInfo
        {
            OperationId declOp = OperationId::invalid();
            std::vector<OperationId> writePorts;
            bool live = false;
            bool declaredRoot = false;
            uint32_t rootMask = 0;
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

        bool isPortValue(const Value &value)
        {
            return value.isInput() || value.isOutput() || value.isInout();
        }

        std::optional<std::string> getAttrString(const Operation &op, std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (const auto *value = std::get_if<std::string>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
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

        std::optional<std::string> getStorageSymbol(const Operation &op)
        {
            switch (op.kind())
            {
            case OperationKind::kRegister:
            case OperationKind::kLatch:
                if (!op.symbolText().empty())
                {
                    return std::string(op.symbolText());
                }
                return std::nullopt;
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterReadArrayPort:
            case OperationKind::kRegisterWritePort:
            case OperationKind::kRegisterWriteMaskPort:
            case OperationKind::kRegisterWriteArrayPort:
            case OperationKind::kRegisterWriteArrayMaskPort:
            case OperationKind::kRegisterFillPort:
                return getAttrString(op, "regSymbol");
            case OperationKind::kLatchReadPort:
            case OperationKind::kLatchWritePort:
                return getAttrString(op, "latchSymbol");
            default:
                return std::nullopt;
            }
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
        PassResult result;
        bool anyChanged = false;
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));
        std::size_t changedGraphs = 0;

        for (const auto &entry : design().graphs())
        {
            wolvrix::lib::grh::Graph &graph = *entry.second;
            bool graphChanged = false;

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
            std::vector<OperationId> defOpByValue;
            if (maxValueIndex > 0)
            {
                isPort.assign(static_cast<std::size_t>(maxValueIndex + 1), 0);
                isDeclaredValue.assign(static_cast<std::size_t>(maxValueIndex + 1), 0);
                liveValueMask.assign(static_cast<std::size_t>(maxValueIndex + 1), 0);
                defOpByValue.assign(static_cast<std::size_t>(maxValueIndex + 1), OperationId::invalid());
            }
            if (maxOpIndex > 0)
            {
                liveOpMask.assign(static_cast<std::size_t>(maxOpIndex + 1), 0);
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
                std::vector<ValueId> operands;
                std::vector<ValueId> results;
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
                const wolvrix::lib::grh::Operation op = graph.getOperation(opId);
                OpInfo info;
                info.id = opId;
                info.kind = op.kind();
                info.operands = std::vector<ValueId>(op.operands().begin(), op.operands().end());
                info.results = std::vector<ValueId>(op.results().begin(), op.results().end());
                const std::size_t infoIndex = ops.size();
                ops.push_back(std::move(info));
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
                const Operation op = graph.getOperation(info.id);
                switch (info.kind)
                {
                case OperationKind::kRegister:
                case OperationKind::kLatch: {
                    auto sym = getStorageSymbol(op);
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
                    auto sym = getStorageSymbol(op);
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
            };
            struct OpWorkItem
            {
                OperationId id = OperationId::invalid();
                uint32_t deltaMask = 0;
            };

            auto markValueLive = [&](ValueId valueId, uint32_t mask, std::deque<ValueWorkItem> &worklist) {
                if (!valueId.valid() || valueId.index >= liveValueMask.size())
                {
                    return;
                }
                const uint32_t prevMask = liveValueMask[valueId.index];
                const uint32_t deltaMask = mask & ~prevMask;
                if (deltaMask == 0)
                {
                    return;
                }
                liveValueMask[valueId.index] = prevMask | deltaMask;
                worklist.push_back({valueId, deltaMask});
            };
            auto markOpLive = [&](OperationId opId, uint32_t mask, std::deque<OpWorkItem> &worklist) {
                if (!opId.valid() || opId.index >= liveOpMask.size())
                {
                    return;
                }
                const uint32_t prevMask = liveOpMask[opId.index];
                const uint32_t deltaMask = mask & ~prevMask;
                if (deltaMask == 0)
                {
                    return;
                }
                liveOpMask[opId.index] = prevMask | deltaMask;
                worklist.push_back({opId, deltaMask});
            };
            auto markStorageLive = [&](std::unordered_map<std::string, StorageInfo> &map,
                                       const std::string &sym,
                                       uint32_t mask,
                                       std::deque<OpWorkItem> &worklist) {
                auto it = map.find(sym);
                if (it == map.end())
                {
                    return;
                }
                it->second.live = true;
                const uint32_t deltaMask = mask & ~it->second.rootMask;
                if (deltaMask == 0)
                {
                    return;
                }
                it->second.rootMask |= deltaMask;
                if (it->second.declOp.valid())
                {
                    markOpLive(it->second.declOp, deltaMask, worklist);
                }
                for (const auto writeOp : it->second.writePorts)
                {
                    markOpLive(writeOp, deltaMask, worklist);
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
                    const Operation op = graph.getOperation(opId);
                    auto symText = getStorageSymbol(op);
                    auto *map = storageMapForKind(op.kind(), regInfos, latchInfos);
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
                    markValueLive(valueId, kRootPort, liveValueWorklist);
                }
                if (valueId.index < isDeclaredValue.size() && isDeclaredValue[valueId.index] != 0)
                {
                    markValueLive(valueId, kRootDeclaredValue, liveValueWorklist);
                }
            }

            for (auto &[sym, info] : regInfos)
            {
                if (info.declaredRoot)
                {
                    markStorageLive(regInfos, sym, kRootDeclaredStorage, liveOpWorklist);
                }
            }
            for (auto &[sym, info] : latchInfos)
            {
                if (info.declaredRoot)
                {
                    markStorageLive(latchInfos, sym, kRootDeclaredStorage, liveOpWorklist);
                }
            }

            for (const auto &info : ops)
            {
                if (isObservableBoundaryOp(info.kind))
                {
                    const uint32_t rootMask = rootMaskForObservableBoundaryOp(info.kind);
                    if (rootMask != 0)
                    {
                        markOpLive(info.id, rootMask, liveOpWorklist);
                    }
                }
            }

            while (!liveValueWorklist.empty() || !liveOpWorklist.empty())
            {
                while (!liveValueWorklist.empty())
                {
                    const ValueWorkItem item = liveValueWorklist.front();
                    liveValueWorklist.pop_front();
                    const ValueId valueId = item.id;
                    if (!valueId.valid() || valueId.index >= defOpByValue.size())
                    {
                        continue;
                    }
                    const OperationId defOp = defOpByValue[valueId.index];
                    if (defOp.valid())
                    {
                        markOpLive(defOp, item.deltaMask, liveOpWorklist);
                    }
                }

                while (!liveOpWorklist.empty())
                {
                    const OpWorkItem item = liveOpWorklist.front();
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
                    const Operation op = graph.getOperation(opId);
                    if (isStateReadOp(info.kind) || isStateWriteOp(info.kind) || isStateDeclOp(info.kind))
                    {
                        auto sym = getStorageSymbol(op);
                        auto *map = storageMapForKind(info.kind, regInfos, latchInfos);
                        if (sym && map)
                        {
                            markStorageLive(*map, *sym, item.deltaMask, liveOpWorklist);
                        }
                    }

                    for (const auto operandId : info.operands)
                    {
                        markValueLive(operandId, item.deltaMask, liveValueWorklist);
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

            for (const auto &info : ops)
            {
                if (info.id.valid() && info.id.index < liveOpMask.size() && liveOpMask[info.id.index] != 0)
                {
                    continue;
                }
                if (graph.eraseOpUnchecked(info.id))
                {
                    graphChanged = true;
                }
            }

            std::vector<ValueId> deadValues;
            for (const auto valueId : valueSpan)
            {
                if (!valueId.valid())
                {
                    continue;
                }
                const Value value = graph.getValue(valueId);
                if (isPortValue(value))
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
                if (!value.users().empty())
                {
                    continue;
                }
                const OperationId defOp = value.definingOp();
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
                }
            }

            anyChanged = anyChanged || graphChanged;
            if (graphChanged)
            {
                ++changedGraphs;
            }
        }

        result.changed = anyChanged;
        result.failed = false;
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
        }
        return result;
    }

} // namespace wolvrix::lib::transform
