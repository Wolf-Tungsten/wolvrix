#include "transform/demo_stats.hpp"

#include "core/grh.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::transform
{

    StatsPass::StatsPass()
        : Pass("stats", "operation-value-stats", "Count graphs, operations, and values for diagnostics"),
          options_({})
    {
    }

    StatsPass::StatsPass(StatsOptions options)
        : Pass("stats", "operation-value-stats", "Count graphs, operations, and values for diagnostics"),
          options_(std::move(options))
    {
    }

    PassResult StatsPass::run()
    {
        std::size_t graphCount = 0;
        std::size_t opCount = 0;
        std::size_t valueCount = 0;
        uint64_t valueBitwidthTotal = 0;
        uint64_t registerBitwidthTotal = 0;
        uint64_t latchBitwidthTotal = 0;
        uint64_t memoryPortBitwidthTotal = 0;
        uint64_t memoryCapacityTotal = 0;
        std::map<int32_t, std::size_t> valueWidthCounts;
        std::map<std::string, std::size_t> opKindCounts;
        std::map<int64_t, std::size_t> registerWidthCounts;
        std::map<int64_t, std::size_t> latchWidthCounts;
        std::map<int64_t, std::size_t> memoryWidthCounts;
        std::map<int64_t, std::size_t> memoryRowCounts;
        std::map<uint64_t, std::size_t> memoryCapacityCounts;
        std::map<uint64_t, std::size_t> coneDepthCounts;
        std::map<uint64_t, std::size_t> coneSizeCounts;
        std::map<uint64_t, std::size_t> coneFaninCounts;
        std::map<uint64_t, std::size_t> combResultUserCounts;
        std::map<uint64_t, std::size_t> combFanoutStatefulCounts;
        struct MaxSymbolEntry
        {
            bool valid = false;
            uint64_t value = 0;
            std::vector<std::string> symbols;
        };
        MaxSymbolEntry maxValueWidth;
        MaxSymbolEntry maxRegisterWidth;
        MaxSymbolEntry maxLatchWidth;
        MaxSymbolEntry maxMemoryWidth;
        MaxSymbolEntry maxMemoryCapacity;
        MaxSymbolEntry maxWriteportConeDepth;
        MaxSymbolEntry maxWriteportConeSize;
        MaxSymbolEntry maxWriteportConeFanin;
        MaxSymbolEntry maxCombResultUsers;
        MaxSymbolEntry maxCombFanout;
        MaxSymbolEntry maxReadportFanout;
        auto getIntAttr = [](std::span<const wolvrix::lib::grh::AttrKV> attrs,
                             std::string_view key) -> std::optional<int64_t>
        {
            for (const auto &attr : attrs)
            {
                if (attr.key == key)
                {
                    if (const auto *value = std::get_if<int64_t>(&attr.value))
                    {
                        return *value;
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        };
        auto getIntArrayAttr = [](std::span<const wolvrix::lib::grh::AttrKV> attrs,
                                  std::string_view key) -> const std::vector<int64_t> *
        {
            for (const auto &attr : attrs)
            {
                if (attr.key == key)
                {
                    return std::get_if<std::vector<int64_t>>(&attr.value);
                }
            }
            return nullptr;
        };

        auto isCombinationalOp = [](wolvrix::lib::grh::OperationKind kind) -> bool
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegister:
            case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
            case wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteMaskPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayMaskPort:
            case wolvrix::lib::grh::OperationKind::kRegisterFillPort:
            case wolvrix::lib::grh::OperationKind::kLatch:
            case wolvrix::lib::grh::OperationKind::kLatchReadPort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
            case wolvrix::lib::grh::OperationKind::kInstance:
            case wolvrix::lib::grh::OperationKind::kBlackbox:
            case wolvrix::lib::grh::OperationKind::kSystemFunction:
            case wolvrix::lib::grh::OperationKind::kSystemTask:
            case wolvrix::lib::grh::OperationKind::kDpicImport:
            case wolvrix::lib::grh::OperationKind::kDpicCall:
            case wolvrix::lib::grh::OperationKind::kXMRRead:
            case wolvrix::lib::grh::OperationKind::kXMRWrite:
                return false;
            default:
                return true;
            }
        };
        auto isStatefulWritePort = [](wolvrix::lib::grh::OperationKind kind) -> bool
        {
            switch (kind)
            {
            case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteMaskPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayPort:
            case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayMaskPort:
            case wolvrix::lib::grh::OperationKind::kRegisterFillPort:
            case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                return true;
            default:
                return false;
            }
        };
        auto qualifyGraphSymbol = [](const wolvrix::lib::grh::Graph *graph) -> std::string
        {
            if (!graph)
            {
                return "graph";
            }
            const auto &symbol = graph->symbol();
            if (!symbol.empty())
            {
                return symbol;
            }
            return "graph#" + std::to_string(graph->id().index);
        };
        auto qualifyValueSymbol = [&](const wolvrix::lib::grh::Graph *graph,
                                      const wolvrix::lib::grh::ValueId &valueId) -> std::string
        {
            std::string symbol;
            if (graph && valueId.valid())
            {
                const auto sym = graph->valueSymbol(valueId);
                if (sym.valid())
                {
                    symbol = std::string(graph->symbolText(sym));
                }
            }
            if (symbol.empty())
            {
                symbol = "value#" + std::to_string(valueId.index);
            }
            return qualifyGraphSymbol(graph) + "::" + symbol;
        };
        auto qualifyOpSymbol = [&](const wolvrix::lib::grh::Graph *graph,
                                   const wolvrix::lib::grh::OperationId &opId) -> std::string
        {
            std::string symbol;
            if (graph && opId.valid())
            {
                const auto sym = graph->operationSymbol(opId);
                if (sym.valid())
                {
                    symbol = std::string(graph->symbolText(sym));
                }
            }
            if (symbol.empty())
            {
                symbol = "op#" + std::to_string(opId.index);
            }
            return qualifyGraphSymbol(graph) + "::" + symbol;
        };
        auto updateMax = [](MaxSymbolEntry &entry, uint64_t value, const std::string &symbol)
        {
            if (!entry.valid || value > entry.value)
            {
                entry.valid = true;
                entry.value = value;
                entry.symbols.clear();
                entry.symbols.push_back(symbol);
                return;
            }
            if (value == entry.value)
            {
                if (std::find(entry.symbols.begin(), entry.symbols.end(), symbol) == entry.symbols.end())
                {
                    entry.symbols.push_back(symbol);
                }
            }
        };
        auto mergeMax = [&](MaxSymbolEntry &dst, const MaxSymbolEntry &src)
        {
            if (!src.valid)
            {
                return;
            }
            if (!dst.valid || src.value > dst.value)
            {
                dst = src;
                return;
            }
            if (src.value == dst.value)
            {
                for (const auto &symbol : src.symbols)
                {
                    if (std::find(dst.symbols.begin(), dst.symbols.end(), symbol) == dst.symbols.end())
                    {
                        dst.symbols.push_back(symbol);
                    }
                }
            }
        };

        struct ConeTask
        {
            const wolvrix::lib::grh::Graph *graph = nullptr;
            wolvrix::lib::grh::ValueId root{};
        };
        struct GraphContext
        {
            const wolvrix::lib::grh::Graph *graph = nullptr;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::size_t,
                               wolvrix::lib::grh::ValueIdHash>
                outputPortCounts;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::size_t,
                               wolvrix::lib::grh::ValueIdHash>
                declaredValueCounts;
        };
        struct FanoutTask
        {
            const GraphContext *ctx = nullptr;
            wolvrix::lib::grh::OperationId op{};
        };
        struct ConeLocal
        {
            std::unordered_map<uint64_t, std::size_t> depthCounts;
            std::unordered_map<uint64_t, std::size_t> sizeCounts;
            std::unordered_map<uint64_t, std::size_t> faninCounts;
            MaxSymbolEntry maxDepth;
            MaxSymbolEntry maxSize;
            MaxSymbolEntry maxFanin;
            std::unordered_map<wolvrix::lib::grh::ValueId, uint64_t,
                               wolvrix::lib::grh::ValueIdHash>
                depthMemo;
            std::unordered_map<wolvrix::lib::grh::ValueId, uint8_t,
                               wolvrix::lib::grh::ValueIdHash>
                depthState;
        };
        struct FanoutLocal
        {
            std::unordered_map<uint64_t, std::size_t> counts;
            MaxSymbolEntry maxFanout;
        };

        std::vector<GraphContext> graphContexts;
        graphContexts.reserve(design().graphs().size());
        for (const auto &entry : design().graphs())
        {
            GraphContext ctx;
            ctx.graph = entry.second.get();
            for (const auto &port : ctx.graph->outputPorts())
            {
                ++ctx.outputPortCounts[port.value];
            }
            for (const auto &port : ctx.graph->inoutPorts())
            {
                ++ctx.outputPortCounts[port.out];
            }
            for (const auto &symbol : ctx.graph->declaredSymbols())
            {
                const auto valueId = ctx.graph->findValue(symbol);
                if (valueId.valid())
                {
                    ++ctx.declaredValueCounts[valueId];
                }
            }
            graphContexts.push_back(std::move(ctx));
        }
        for (const auto &ctx : graphContexts)
        {
            (void)ctx.graph->operations();
            (void)ctx.graph->values();
        }

        std::vector<ConeTask> coneTasks;
        std::vector<FanoutTask> combFanoutTasks;
        std::vector<FanoutTask> readportFanoutTasks;

        for (const auto &ctx : graphContexts)
        {
            ++graphCount;
            const auto *graph = ctx.graph;
            for (const auto &opId : graph->operations())
            {
                ++opCount;
                const auto kind = graph->opKind(opId);
                const std::string kindText(toString(kind));
                ++opKindCounts[kindText];
                const auto attrs = graph->opAttrs(opId);
                const auto width = getIntAttr(attrs, "width");
                switch (kind)
                {
                case wolvrix::lib::grh::OperationKind::kRegister:
                    if (width && *width > 0)
                    {
                        ++registerWidthCounts[*width];
                        registerBitwidthTotal += static_cast<uint64_t>(*width);
                        const uint64_t widthValue = static_cast<uint64_t>(*width);
                        if (!maxRegisterWidth.valid || widthValue >= maxRegisterWidth.value)
                        {
                            updateMax(maxRegisterWidth, widthValue, qualifyOpSymbol(graph, opId));
                        }
                        // Array registers (shape attr) also feed the memory
                        // statistics the retired kMemory produced.
                        if (const auto *shapeAttr = getIntArrayAttr(attrs, "shape"))
                        {
                            int64_t elementCount = 1;
                            for (const int64_t dim : *shapeAttr)
                            {
                                elementCount *= dim;
                            }
                            if (elementCount > 0 && *width % elementCount == 0)
                            {
                                const int64_t elemWidth = *width / elementCount;
                                ++memoryWidthCounts[elemWidth];
                                ++memoryRowCounts[elementCount];
                                if (!maxMemoryWidth.valid ||
                                    static_cast<uint64_t>(elemWidth) >= maxMemoryWidth.value)
                                {
                                    updateMax(maxMemoryWidth, static_cast<uint64_t>(elemWidth),
                                              qualifyOpSymbol(graph, opId));
                                }
                                const uint64_t capacity = widthValue;
                                memoryCapacityTotal += capacity;
                                ++memoryCapacityCounts[capacity];
                                if (!maxMemoryCapacity.valid || capacity >= maxMemoryCapacity.value)
                                {
                                    updateMax(maxMemoryCapacity, capacity,
                                              qualifyOpSymbol(graph, opId));
                                }
                            }
                        }
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kLatch:
                    if (width && *width > 0)
                    {
                        ++latchWidthCounts[*width];
                        latchBitwidthTotal += static_cast<uint64_t>(*width);
                        const uint64_t widthValue = static_cast<uint64_t>(*width);
                        if (!maxLatchWidth.valid || widthValue >= maxLatchWidth.value)
                        {
                            updateMax(maxLatchWidth, widthValue, qualifyOpSymbol(graph, opId));
                        }
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort:
                    {
                        const auto results = graph->opResults(opId);
                        if (!results.empty())
                        {
                            const int32_t dataWidth = graph->valueWidth(results[0]);
                            if (dataWidth > 0)
                            {
                                memoryPortBitwidthTotal += static_cast<uint64_t>(dataWidth);
                            }
                        }
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kRegisterWritePort:
                case wolvrix::lib::grh::OperationKind::kRegisterWriteMaskPort:
                case wolvrix::lib::grh::OperationKind::kLatchWritePort:
                    {
                        const auto operands = graph->opOperands(opId);
                        if (operands.size() >= 2)
                        {
                            coneTasks.push_back({graph, operands[0]});
                            coneTasks.push_back({graph, operands[1]});
                        }
                    }
                    break;
                case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayPort:
                case wolvrix::lib::grh::OperationKind::kRegisterWriteArrayMaskPort:
                case wolvrix::lib::grh::OperationKind::kRegisterFillPort:
                    {
                        const auto operands = graph->opOperands(opId);
                        std::size_t dataIndex = 1;
                        if (kind != wolvrix::lib::grh::OperationKind::kRegisterFillPort)
                        {
                            const wolvrix::lib::grh::Operation writeOp = graph->getOperation(opId);
                            const std::optional<std::size_t> eventStart =
                                wolvrix::lib::grh::firstEventOperandIndex(writeOp);
                            if (!eventStart)
                            {
                                break;
                            }
                            dataIndex = *eventStart -
                                        (kind == wolvrix::lib::grh::OperationKind::kRegisterWriteArrayMaskPort ? 2 : 1);
                        }
                        if (operands.size() > dataIndex)
                        {
                            const int32_t dataWidth = graph->valueWidth(operands[dataIndex]);
                            if (dataWidth > 0)
                            {
                                memoryPortBitwidthTotal += static_cast<uint64_t>(dataWidth);
                            }
                            coneTasks.push_back({graph, operands[0]});
                            coneTasks.push_back({graph, operands[dataIndex]});
                        }
                    }
                    break;
                default:
                    break;
                }

                if (isCombinationalOp(kind))
                {
                    combFanoutTasks.push_back({&ctx, opId});
                    for (const auto resultId : graph->opResults(opId))
                    {
                        if (!resultId.valid())
                        {
                            continue;
                        }
                        const uint64_t userCount =
                            static_cast<uint64_t>(graph->getValue(resultId).users().size());
                        ++combResultUserCounts[userCount];
                        updateMax(maxCombResultUsers, userCount,
                                  qualifyValueSymbol(graph, resultId));
                    }
                }
                switch (kind)
                {
                case wolvrix::lib::grh::OperationKind::kRegisterReadPort:
                case wolvrix::lib::grh::OperationKind::kRegisterReadArrayPort:
                case wolvrix::lib::grh::OperationKind::kLatchReadPort:
                    readportFanoutTasks.push_back({&ctx, opId});
                    break;
                default:
                    break;
                }
            }
            for (const auto &valueId : graph->values())
            {
                ++valueCount;
                const int32_t width = graph->valueWidth(valueId);
                ++valueWidthCounts[width];
                if (width > 0)
                {
                    valueBitwidthTotal += static_cast<uint64_t>(width);
                    const uint64_t widthValue = static_cast<uint64_t>(width);
                    if (!maxValueWidth.valid || widthValue >= maxValueWidth.value)
                    {
                        updateMax(maxValueWidth, widthValue,
                                  qualifyValueSymbol(graph, valueId));
                    }
                }
            }
        }

        const std::size_t maxThreads =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::thread::hardware_concurrency()));
        auto runParallel = [&](const auto &tasks, auto &locals, auto &&func)
        {
            if (tasks.empty())
            {
                return;
            }
            const std::size_t threadCount = std::min<std::size_t>(maxThreads, tasks.size());
            locals.resize(threadCount);
            std::atomic<std::size_t> next{0};
            std::vector<std::thread> threads;
            threads.reserve(threadCount);
            for (std::size_t t = 0; t < threadCount; ++t)
            {
                threads.emplace_back([&, t]() {
                    for (;;)
                    {
                        const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= tasks.size())
                        {
                            return;
                        }
                        func(tasks[index], locals[t]);
                    }
                });
            }
            for (auto &thread : threads)
            {
                thread.join();
            }
        };
        auto mergeCounts = [](auto &dst, const auto &src)
        {
            for (const auto &entry : src)
            {
                dst[entry.first] += entry.second;
            }
        };

        std::vector<ConeLocal> coneLocals;
        runParallel(
            coneTasks,
            coneLocals,
            [&](const ConeTask &task, ConeLocal &local) {
                if (!task.root.valid())
                {
                    return;
                }
                auto computeDepth = [&](auto &&self, wolvrix::lib::grh::ValueId valueId) -> uint64_t
                {
                    if (!valueId.valid())
                    {
                        return 0;
                    }
                    auto memoIt = local.depthMemo.find(valueId);
                    if (memoIt != local.depthMemo.end())
                    {
                        return memoIt->second;
                    }
                    auto stateIt = local.depthState.find(valueId);
                    if (stateIt != local.depthState.end() && stateIt->second == 1)
                    {
                        local.depthState[valueId] = 2;
                        local.depthMemo[valueId] = 0;
                        return 0;
                    }
                    local.depthState[valueId] = 1;
                    uint64_t depth = 0;
                    const auto defOpId = task.graph->valueDef(valueId);
                    if (defOpId.valid())
                    {
                        const auto defKind = task.graph->opKind(defOpId);
                        if (isCombinationalOp(defKind))
                        {
                            uint64_t maxDepth = 0;
                            for (const auto &operand : task.graph->opOperands(defOpId))
                            {
                                maxDepth = std::max(maxDepth, self(self, operand));
                            }
                            depth = maxDepth + 1;
                        }
                    }
                    local.depthState[valueId] = 2;
                    local.depthMemo[valueId] = depth;
                    return depth;
                };

                std::unordered_set<wolvrix::lib::grh::OperationId,
                                   wolvrix::lib::grh::OperationIdHash>
                    visitedOps;
                std::unordered_set<wolvrix::lib::grh::ValueId,
                                   wolvrix::lib::grh::ValueIdHash>
                    visitedValues;
                std::unordered_set<wolvrix::lib::grh::ValueId,
                                   wolvrix::lib::grh::ValueIdHash>
                    leafValues;
                std::vector<wolvrix::lib::grh::ValueId> stack;
                stack.push_back(task.root);
                while (!stack.empty())
                {
                    const auto valueId = stack.back();
                    stack.pop_back();
                    if (!valueId.valid())
                    {
                        continue;
                    }
                    if (!visitedValues.insert(valueId).second)
                    {
                        continue;
                    }
                    const auto defOpId = task.graph->valueDef(valueId);
                    if (!defOpId.valid())
                    {
                        leafValues.insert(valueId);
                        continue;
                    }
                    const auto defKind = task.graph->opKind(defOpId);
                    if (!isCombinationalOp(defKind))
                    {
                        leafValues.insert(valueId);
                        continue;
                    }
                    if (!visitedOps.insert(defOpId).second)
                    {
                        continue;
                    }
                    for (const auto &operand : task.graph->opOperands(defOpId))
                    {
                        stack.push_back(operand);
                    }
                }
                const uint64_t depth = computeDepth(computeDepth, task.root);
                const uint64_t size = static_cast<uint64_t>(visitedOps.size());
                const uint64_t fanin = static_cast<uint64_t>(leafValues.size());
                ++local.depthCounts[depth];
                ++local.sizeCounts[size];
                ++local.faninCounts[fanin];
                const auto rootSymbol = qualifyValueSymbol(task.graph, task.root);
                updateMax(local.maxDepth, depth, rootSymbol);
                updateMax(local.maxSize, size, rootSymbol);
                updateMax(local.maxFanin, fanin, rootSymbol);
            });
        for (const auto &local : coneLocals)
        {
            mergeCounts(coneDepthCounts, local.depthCounts);
            mergeCounts(coneSizeCounts, local.sizeCounts);
            mergeCounts(coneFaninCounts, local.faninCounts);
            mergeMax(maxWriteportConeDepth, local.maxDepth);
            mergeMax(maxWriteportConeSize, local.maxSize);
            mergeMax(maxWriteportConeFanin, local.maxFanin);
        }

        auto computeSinkFanout = [&](const GraphContext *ctx,
                                     std::span<const wolvrix::lib::grh::ValueId> roots) -> uint64_t
        {
            const auto *graph = ctx->graph;
            std::unordered_set<wolvrix::lib::grh::OperationId,
                               wolvrix::lib::grh::OperationIdHash>
                drivenSinks;
            std::unordered_set<wolvrix::lib::grh::ValueId,
                               wolvrix::lib::grh::ValueIdHash>
                visitedValues;
            std::unordered_set<wolvrix::lib::grh::OperationId,
                               wolvrix::lib::grh::OperationIdHash>
                visitedOps;
            std::unordered_set<wolvrix::lib::grh::ValueId,
                               wolvrix::lib::grh::ValueIdHash>
                sinkSeen;
            std::vector<wolvrix::lib::grh::ValueId> stack;
            stack.reserve(roots.size());
            for (const auto &root : roots)
            {
                if (root.valid())
                {
                    stack.push_back(root);
                }
            }
            uint64_t outputCount = 0;
            while (!stack.empty())
            {
                const auto valueId = stack.back();
                stack.pop_back();
                if (!valueId.valid())
                {
                    continue;
                }
                if (!visitedValues.insert(valueId).second)
                {
                    continue;
                }
                const auto outputIt = ctx->outputPortCounts.find(valueId);
                const auto declaredIt = ctx->declaredValueCounts.find(valueId);
                if (outputIt != ctx->outputPortCounts.end() ||
                    declaredIt != ctx->declaredValueCounts.end())
                {
                    if (sinkSeen.insert(valueId).second)
                    {
                        if (outputIt != ctx->outputPortCounts.end())
                        {
                            outputCount += static_cast<uint64_t>(outputIt->second);
                        }
                        else if (declaredIt != ctx->declaredValueCounts.end())
                        {
                            outputCount += static_cast<uint64_t>(declaredIt->second);
                        }
                    }
                }
                const auto value = graph->getValue(valueId);
                for (const auto &user : value.users())
                {
                    const auto userOpId = user.operation;
                    const auto userKind = graph->opKind(userOpId);
                    if (!isCombinationalOp(userKind))
                    {
                        drivenSinks.insert(userOpId);
                        continue;
                    }
                    if (!visitedOps.insert(userOpId).second)
                    {
                        continue;
                    }
                    for (const auto &res : graph->opResults(userOpId))
                    {
                        if (res.valid())
                        {
                            stack.push_back(res);
                        }
                    }
                }
            }
            return static_cast<uint64_t>(drivenSinks.size()) + outputCount;
        };

        std::vector<FanoutLocal> combFanoutLocals;
        runParallel(
            combFanoutTasks,
            combFanoutLocals,
            [&](const FanoutTask &task, FanoutLocal &local) {
                if (!task.ctx || !task.op.valid())
                {
                    return;
                }
                const auto results = task.ctx->graph->opResults(task.op);
                const uint64_t fanout = computeSinkFanout(task.ctx, results);
                ++local.counts[fanout];
                updateMax(local.maxFanout, fanout,
                          qualifyOpSymbol(task.ctx->graph, task.op));
            });
        for (const auto &local : combFanoutLocals)
        {
            mergeCounts(combFanoutStatefulCounts, local.counts);
            mergeMax(maxCombFanout, local.maxFanout);
        }

        std::map<uint64_t, std::size_t> readportFanoutCounts;
        std::vector<FanoutLocal> readportFanoutLocals;
        runParallel(
            readportFanoutTasks,
            readportFanoutLocals,
            [&](const FanoutTask &task, FanoutLocal &local) {
                if (!task.ctx || !task.op.valid())
                {
                    return;
                }
                const auto results = task.ctx->graph->opResults(task.op);
                const uint64_t fanout = computeSinkFanout(task.ctx, results);
                ++local.counts[fanout];
                updateMax(local.maxFanout, fanout,
                          qualifyOpSymbol(task.ctx->graph, task.op));
            });
        for (const auto &local : readportFanoutLocals)
        {
            mergeCounts(readportFanoutCounts, local.counts);
            mergeMax(maxReadportFanout, local.maxFanout);
        }

        std::ostringstream oss;
        oss << "{";
        bool firstField = true;
        auto appendScalar = [&](std::string_view name, uint64_t value)
        {
            if (!firstField)
            {
                oss << ",";
            }
            oss << "\"" << name << "\":" << value;
            firstField = false;
        };
        auto appendJsonMap = [&](std::string_view name, const auto &entries)
        {
            if (!firstField)
            {
                oss << ",";
            }
            oss << "\"" << name << "\":{";
            bool first = true;
            for (const auto &entry : entries)
            {
                if (!first)
                {
                    oss << ",";
                }
                oss << "\"" << entry.first << "\":" << entry.second;
                first = false;
            }
            oss << "}";
            firstField = false;
        };
        auto appendEscapedString = [&](std::string_view text)
        {
            for (char ch : text)
            {
                switch (ch)
                {
                case '\\':
                    oss << "\\\\";
                    break;
                case '"':
                    oss << "\\\"";
                    break;
                case '\n':
                    oss << "\\n";
                    break;
                case '\r':
                    oss << "\\r";
                    break;
                case '\t':
                    oss << "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20)
                    {
                        oss << "\\u00";
                        const char *digits = "0123456789abcdef";
                        oss << digits[(ch >> 4) & 0x0f];
                        oss << digits[ch & 0x0f];
                    }
                    else
                    {
                        oss << ch;
                    }
                    break;
                }
            }
        };
        auto appendMaxSymbolEntry = [&](std::string_view name, const MaxSymbolEntry &entry, bool &first)
        {
            if (!first)
            {
                oss << ",";
            }
            oss << "\"" << name << "\":{";
            oss << "\"max\":" << (entry.valid ? entry.value : 0) << ",\"symbols\":[";
            bool firstSymbol = true;
            for (const auto &symbol : entry.symbols)
            {
                if (!firstSymbol)
                {
                    oss << ",";
                }
                oss << "\"";
                appendEscapedString(symbol);
                oss << "\"";
                firstSymbol = false;
            }
            oss << "]}";
            first = false;
        };

        appendScalar("graph_count", graphCount);
        appendScalar("operation_count", opCount);
        appendScalar("value_count", valueCount);
        appendScalar("value_bitwidth_total", valueBitwidthTotal);
        appendScalar("register_bitwidth_total", registerBitwidthTotal);
        appendScalar("latch_bitwidth_total", latchBitwidthTotal);
        appendScalar("memory_port_bitwidth_total", memoryPortBitwidthTotal);
        appendScalar("memory_capacity_total", memoryCapacityTotal);
        appendJsonMap("value_widths", valueWidthCounts);
        appendJsonMap("operation_kinds", opKindCounts);
        appendJsonMap("register_widths", registerWidthCounts);
        appendJsonMap("latch_widths", latchWidthCounts);
        appendJsonMap("memory_widths", memoryWidthCounts);
        appendJsonMap("memory_rows", memoryRowCounts);
        appendJsonMap("memory_capacity_bits", memoryCapacityCounts);
        appendJsonMap("writeport_cone_depths", coneDepthCounts);
        appendJsonMap("writeport_cone_sizes", coneSizeCounts);
        appendJsonMap("writeport_cone_fanins", coneFaninCounts);
        appendJsonMap("comb_result_user_counts", combResultUserCounts);
        appendJsonMap("comb_op_fanout_sinks", combFanoutStatefulCounts);
        appendJsonMap("readport_fanout_sinks", readportFanoutCounts);
        if (!firstField)
        {
            oss << ",";
        }
        oss << "\"max_symbols\":{";
        bool firstMax = true;
        appendMaxSymbolEntry("value_widths", maxValueWidth, firstMax);
        appendMaxSymbolEntry("register_widths", maxRegisterWidth, firstMax);
        appendMaxSymbolEntry("latch_widths", maxLatchWidth, firstMax);
        appendMaxSymbolEntry("memory_widths", maxMemoryWidth, firstMax);
        appendMaxSymbolEntry("memory_capacity_bits", maxMemoryCapacity, firstMax);
        appendMaxSymbolEntry("writeport_cone_depths", maxWriteportConeDepth, firstMax);
        appendMaxSymbolEntry("writeport_cone_sizes", maxWriteportConeSize, firstMax);
        appendMaxSymbolEntry("writeport_cone_fanins", maxWriteportConeFanin, firstMax);
        appendMaxSymbolEntry("comb_result_user_counts", maxCombResultUsers, firstMax);
        appendMaxSymbolEntry("comb_op_fanout_sinks", maxCombFanout, firstMax);
        appendMaxSymbolEntry("readport_fanout_sinks", maxReadportFanout, firstMax);
        oss << "}";
        firstField = false;
        oss << "}";

        std::string message = oss.str();

        if (!options_.outputKey.empty())
        {
            setSessionValue(options_.outputKey, message, "stats");
        }

        info(message);

        PassResult result;
        result.changed = false;
        result.failed = false;
        result.artifacts.push_back(message);

        return result;
    }

} // namespace wolvrix::lib::transform
