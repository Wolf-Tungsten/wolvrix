#include "transform/hier_flatten.hpp"

#include "core/detail/block_executor.hpp"
#include "core/grh.hpp"
#include "core/transform.hpp"
#include "ingest/node_attributes.hpp"
#include "slang/util/SmallVector.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform
{

    namespace
    {
        using FlattenClock = std::chrono::steady_clock;

        struct FlattenMetrics
        {
            std::size_t sourceCalls = 0;
            std::size_t instanceCalls = 0;
            std::size_t valuesVisited = 0;
            std::size_t valuesCloned = 0;
            std::size_t operationsVisited = 0;
            std::size_t operationsCloned = 0;
            std::size_t instanceOperations = 0;
            std::size_t annotatedOperations = 0;
            std::size_t qualificationLookups = 0;
            std::size_t qualificationHits = 0;
            std::size_t qualificationMisses = 0;
            std::size_t qualificationInputBytes = 0;
            std::size_t qualificationOutputBytes = 0;
            std::size_t pendingAttributes = 0;
            std::size_t generateGroups = 0;
            std::size_t semanticNodesCopied = 0;
            std::size_t semanticNameBytesCopied = 0;
            std::chrono::nanoseconds semanticCopy{};
            std::chrono::nanoseconds freeze{};
            std::chrono::nanoseconds capacity{};
            std::chrono::nanoseconds topStamp{};
            std::chrono::nanoseconds inventory{};
            std::chrono::nanoseconds values{};
            std::chrono::nanoseconds symbols{};
            std::chrono::nanoseconds operations{};
            std::chrono::nanoseconds recursiveInstances{};
            std::chrono::nanoseconds qualification{};
            std::chrono::nanoseconds groups{};
            std::chrono::nanoseconds flush{};
            std::chrono::nanoseconds total{};
        };

        // Every expanded source value has one node, including omitted ports.
        // Union is directed towards the first published target value; choosing
        // a rank representative would change legacy boundary value identities.
        struct GlobalPortUnionFind
        {
            static constexpr std::size_t invalid = std::numeric_limits<std::size_t>::max();
            struct Node
            {
                std::size_t parent = invalid;
                wolvrix::lib::grh::ValueId target;
            };
            std::vector<Node> nodes;
            std::vector<std::size_t> targetNodes;

            std::size_t root(std::size_t node) const noexcept
            {
                while (nodes[node].parent != invalid) node = nodes[node].parent;
                return node;
            }

            void bind(std::size_t node, wolvrix::lib::grh::ValueId target)
            {
                if (node >= nodes.size() || !target.valid())
                    throw std::invalid_argument("Flatten global port binding is outside its planned range");
                if (targetNodes.size() <= target.index)
                    targetNodes.resize(static_cast<std::size_t>(target.index) + 1, invalid);
                const auto prior = targetNodes[target.index];
                if (prior == invalid)
                {
                    nodes[node].target = target;
                    targetNodes[target.index] = node;
                    return;
                }
                const auto representative = root(prior);
                if (nodes[representative].target != target)
                    throw std::invalid_argument("Flatten global port target graph mismatch");
                const auto current = root(node);
                if (current != representative) nodes[current].parent = representative;
            }

            wolvrix::lib::grh::ValueId get(std::size_t node) const noexcept
            {
                return node < nodes.size() ? nodes[root(node)].target : wolvrix::lib::grh::ValueId{};
            }

            void compress()
            {
                for (std::size_t node = 0; node < nodes.size(); ++node)
                {
                    const auto representative = root(node);
                    if (node != representative) nodes[node].parent = representative;
                }
            }
        };

        // Values are allocated with monotonically increasing per-graph IDs.
        // Flattening looks up every operand/result in the current source graph;
        // hashing the full ValueId for each of those accesses dominates large
        // clones. Keep a dense index by ValueId::index instead. Holes are
        // represented by invalid IDs, so this remains safe for graphs that had
        // erased values before they were frozen.
        struct DenseValueMap
        {
            wolvrix::lib::grh::GraphId graph;
            std::vector<wolvrix::lib::grh::ValueId> values;
            // Only port equivalences participate in this directed union-find.
            // Allocate its dense parent array lazily when two distinct source
            // ports actually share a target; ordinary values stay direct.
            std::vector<uint32_t> portParents;
            slang::flat_hash_map<uint32_t, uint32_t, std::hash<uint32_t>> portRepresentatives;
            GlobalPortUnionFind *globalPorts = nullptr;
            std::size_t globalBase = 0;
            std::size_t plannedInstance = GlobalPortUnionFind::invalid;

            DenseValueMap() = default;

            explicit DenseValueMap(const wolvrix::lib::grh::Graph &source) : graph(source.id())
            {
                std::size_t maxIndex = 0;
                for (const auto value : source.values())
                {
                    maxIndex = std::max(maxIndex, static_cast<std::size_t>(value.index));
                }
                values.resize(maxIndex + 1);
            }

            bool contains(wolvrix::lib::grh::ValueId source) const noexcept
            {
                return get(source).valid();
            }

            wolvrix::lib::grh::ValueId get(wolvrix::lib::grh::ValueId source) const noexcept
            {
                if (!source.valid() || source.graph != graph || source.generation != 0 ||
                    source.index >= values.size())
                {
                    return wolvrix::lib::grh::ValueId::invalid();
                }
                if (globalPorts) return globalPorts->get(globalBase + source.index);
                uint32_t index = source.index;
                while (index < portParents.size() && portParents[index]) index = portParents[index];
                return values[index];
            }

            void set(wolvrix::lib::grh::ValueId source,
                     wolvrix::lib::grh::ValueId target)
            {
                if (!source.valid())
                {
                    return;
                }
                if (source.graph != graph || source.generation != 0)
                {
                    throw std::invalid_argument("Flatten mapping requires a value from its source graph");
                }
                if (source.index >= values.size())
                {
                    values.resize(static_cast<std::size_t>(source.index) + 1);
                }
                values[source.index] = target;
                if (globalPorts) globalPorts->bind(globalBase + source.index, target);
            }

            std::size_t node(wolvrix::lib::grh::ValueId source) const noexcept
            {
                return globalPorts && source.valid() && source.graph == graph &&
                    source.generation == 0 && source.index < values.size() ?
                    globalBase + source.index : GlobalPortUnionFind::invalid;
            }

            // Call only after the boundary's exact logical-type check/view.
            // The first legacy target remains the representative; no rank
            // heuristic may choose a different observable target ValueId.
            void bindEquivalentPort(wolvrix::lib::grh::ValueId source,
                                    wolvrix::lib::grh::ValueId target)
            {
                set(source, target);
                if (globalPorts) return;
                const auto [found, inserted] = portRepresentatives.try_emplace(target.index, source.index);
                if (inserted || found->second == source.index) return;
                if (values[found->second] != target)
                    throw std::invalid_argument("Flatten port equivalence requires one destination graph");
                if (portParents.size() < values.size()) portParents.resize(values.size());
                uint32_t root = found->second;
                while (portParents[root]) root = portParents[root];
                portParents[source.index] = root;
            }
        };

        struct PendingAttr
        {
            wolvrix::lib::grh::OperationId op;
            std::string key;
            std::string oldValue;
            wolvrix::lib::grh::SymbolId sourceSymbol;
            bool deferred = false;
        };

        // Each map belongs to one source graph and one expanded instance.
        using SymbolRenameMap = std::unordered_map<uint32_t, wolvrix::lib::grh::SymbolId>;

        struct OperationRun
        {
            std::span<const wolvrix::lib::grh::OperationId> operations;
            bool boundary = false;
        };

        // An instance interrupts its parent's operation allocation order.
        // Keep ordinary interiors as consecutive runs and execute each
        // instance/DPI boundary before allocating the following parent run.
        std::vector<OperationRun> planOperationRuns(const wolvrix::lib::grh::Graph &source)
        {
            const auto operations = source.operations();
            std::vector<OperationRun> runs;
            std::size_t begin = 0;
            for (std::size_t index = 0; index < operations.size(); ++index)
            {
                const auto kind = source.opKind(operations[index]);
                if (kind != wolvrix::lib::grh::OperationKind::kInstance &&
                    kind != wolvrix::lib::grh::OperationKind::kDpicImport)
                {
                    continue;
                }
                if (begin != index) runs.push_back({operations.subspan(begin, index - begin), false});
                runs.push_back({operations.subspan(index, 1), true});
                begin = index + 1;
            }
            if (begin != operations.size()) runs.push_back({operations.subspan(begin), false});
            return runs;
        }

        bool isOpSymbolReferenceAttr(std::string_view key)
        {
            return key == "regSymbol" || key == "latchSymbol" || key == "targetImportSymbol";
        }

        struct InstanceInfo
        {
            std::string moduleName;
            std::string instanceName;
            wolvrix::lib::grh::SemanticId semantic = 0;
            std::vector<wolvrix::lib::grh::ValueId> operands;
            std::vector<wolvrix::lib::grh::ValueId> results;
            std::optional<std::vector<std::string>> inputPortNames;
            std::optional<std::vector<std::string>> outputPortNames;
            std::optional<std::vector<std::string>> inoutPortNames;
        };

        struct GlobalInlinePlan
        {
            enum class EventKind { EnterValues, OpRun, Boundary, Exit };
            struct Event
            {
                EventKind kind;
                std::size_t instance;
                std::size_t run;
                wolvrix::lib::grh::ValueId firstValue;
                wolvrix::lib::grh::OperationId firstOperation;
                std::size_t count = 0;
                std::vector<wolvrix::lib::grh::ValueId> boundaryValues;
                std::vector<wolvrix::lib::grh::OperationId> boundaryOperations;
            };
            struct Instance
            {
                const wolvrix::lib::grh::Graph *source;
                std::size_t nodeBase;
                std::vector<OperationRun> runs;
                std::string prefix;
                SymbolRenameMap opRename;
            };
            struct OperandBlock
            {
                const wolvrix::lib::grh::Graph *source;
                std::span<const wolvrix::lib::grh::OperationId> sourceOps;
                std::size_t instance;
                std::vector<wolvrix::lib::grh::OperationId> operations;
                std::vector<uint32_t> offsets;
                std::vector<std::size_t> nodes;
                std::vector<wolvrix::lib::grh::ValueId> relocated;
                bool consumed = false;
            };
            std::vector<Instance> instances;
            std::vector<Event> events;
            std::vector<OperandBlock> operands;
            std::unordered_map<const wolvrix::lib::grh::Graph *, std::size_t> definitionUses;
            GlobalPortUnionFind ports;
            std::size_t nextInstance = 0;
            std::size_t nextEvent = 0;
            std::size_t currentBoundary = GlobalPortUnionFind::invalid;
            bool enabled = false;
            bool metadataStarted = false;
            std::size_t committedOperandBlocks = 0;

            std::size_t consume(EventKind kind, std::size_t instance, std::size_t run = 0)
            {
                if (!enabled) return GlobalPortUnionFind::invalid;
                if (nextEvent == events.size() || events[nextEvent].kind != kind ||
                    events[nextEvent].instance != instance || events[nextEvent].run != run)
                    throw std::logic_error("Flatten global event order differs from legacy traversal");
                const auto event = nextEvent++;
                if (kind == EventKind::Boundary) currentBoundary = event;
                return event;
            }

            void boundaryValue(wolvrix::lib::grh::ValueId value)
            {
                if (enabled) events[currentBoundary].boundaryValues.push_back(value);
            }

            void boundaryOperation(wolvrix::lib::grh::OperationId op)
            {
                if (enabled) events[currentBoundary].boundaryOperations.push_back(op);
            }

            // Every operand is relocated once, after all directed port unions
            // and all target ranges (including boundary assigns) are fixed.
            void commitOperands(wolvrix::lib::grh::Graph &target,
                                wolvrix::lib::grh::detail::BlockExecutor *executor)
            {
                if (operands.empty()) return;
                ports.compress();
                for (auto &block : operands)
                {
                    if (block.consumed) continue;
                    block.relocated.resize(block.nodes.size());
                    auto *work = &block;
                    executor->submit([this, work] {
                        for (std::size_t i = 0; i < work->nodes.size(); ++i)
                        {
                            work->relocated[i] = ports.get(work->nodes[i]);
                            if (!work->relocated[i].valid())
                                throw std::logic_error("Flatten global operand has no target representative");
                        }
                    });
                }
                executor->wait();
                for (; committedOperandBlocks < operands.size(); ++committedOperandBlocks)
                {
                    const auto &block = operands[committedOperandBlocks];
                    if (block.consumed) continue;
                    target.setOperationBlockOperands(block.operations, block.offsets, block.relocated);
                }
                operands.clear();
            }
        };

        struct InlineState
        {
            wolvrix::lib::grh::Design &design;
            wolvrix::lib::ingest::node::InventoryImportBatch &nodeInventory;
            FlattenMetrics &metrics;
            std::unordered_set<const wolvrix::lib::grh::Graph *> stack;
            std::unordered_map<std::string, int> instanceCounts;
            std::unordered_map<wolvrix::lib::grh::ValueId, std::string,
                               wolvrix::lib::grh::ValueIdHash> renamedValues;
            std::unordered_map<wolvrix::lib::grh::ValueId,
                               std::vector<std::pair<wolvrix::lib::grh::LogicalType, wolvrix::lib::grh::ValueId>>,
                               wolvrix::lib::grh::ValueIdHash> inputTypeViews;
            std::unordered_map<const wolvrix::lib::grh::Graph *,
                               std::unordered_set<uint32_t>> neededOpSymbols;
            // A source definition has identical provenance in every instance.
            // Contexts live only for this flatten operation and share imports
            // between cloned values and operations.
            std::unordered_map<uint64_t, wolvrix::lib::grh::Graph::SrcLocImportContext> srcLocImports;
            std::unordered_map<uint64_t, wolvrix::lib::grh::Graph::BlockImportContext> blockImports;
            std::function<void(const wolvrix::lib::grh::Graph &, std::string)> graphError;
            std::function<void(const wolvrix::lib::grh::Graph &, const wolvrix::lib::grh::Operation &,
                               std::string)> opError;
            HierFlattenOptions::SymProtectMode symProtect = HierFlattenOptions::SymProtectMode::All;
            std::vector<std::string> nodeInstancePath;
            wolvrix::lib::grh::SemanticId currentSemanticParent = 0;
            wolvrix::lib::grh::SemanticId currentInstanceSemantic = 0;
            bool profile = false;
            std::size_t numThreads = 1;
            GlobalInlinePlan plan;
            // Join outstanding tasks before destroying their plan captures,
            // including allocation failures while submitting the next task.
            std::unique_ptr<wolvrix::lib::grh::detail::BlockExecutor> executor;
            bool failed = false;
            bool changed = false;
        };

        std::optional<std::string> getAttrString(const wolvrix::lib::grh::Operation &op,
                                                 std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (auto value = std::get_if<std::string>(&*attr))
            {
                return *value;
            }
            return std::nullopt;
        }

        std::optional<std::vector<std::string>> getAttrStrings(const wolvrix::lib::grh::Operation &op,
                                                                std::string_view key)
        {
            auto attr = op.attr(key);
            if (!attr)
            {
                return std::nullopt;
            }
            if (auto values = std::get_if<std::vector<std::string>>(&*attr))
            {
                return *values;
            }
            return std::nullopt;
        }

        std::optional<std::string_view> getAttrStringBorrowed(
            wolvrix::lib::grh::AttrRange attrs, std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key() == key)
                {
                    if (const auto value = entry.value().get<std::string>())
                    {
                        return *value;
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        std::optional<wolvrix::lib::grh::StringListView> getAttrStringsBorrowed(
            wolvrix::lib::grh::AttrRange attrs, std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key() == key)
                {
                    return entry.value().get<std::vector<std::string>>();
                }
            }
            return std::nullopt;
        }

        std::unordered_set<uint32_t> collectNeededOpSymbols(const wolvrix::lib::grh::Graph &source)
        {
            std::unordered_set<uint32_t> symbols;
            for (const auto opId : source.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                for (const auto &attr : source.opAttrRange(opId))
                {
                    if (!isOpSymbolReferenceAttr(attr.key()))
                    {
                        continue;
                    }
                    if (const auto name = attr.value().get<std::string>())
                    {
                        const auto symbol = source.lookupSymbol(*name);
                        if (symbol.valid())
                        {
                            symbols.insert(symbol.value);
                        }
                    }
                }
            }
            for (const auto &group : source.generateGroups())
            {
                for (const auto member : group.symbols)
                {
                    if (member.valid())
                    {
                        symbols.insert(member.value);
                    }
                }
            }
            return symbols;
        }

        bool hasNamedPortMaps(const wolvrix::lib::grh::Operation &instance,
                              const wolvrix::lib::grh::Graph &source)
        {
            const auto named = [&](std::string_view key, const auto &ports) {
                const auto names = getAttrStrings(instance, key);
                if (!names)
                {
                    return ports.empty();
                }
                if (names->size() != ports.size())
                {
                    return false;
                }
                std::unordered_set<std::string_view> unique;
                unique.reserve(names->size());
                for (const auto &name : *names)
                {
                    unique.insert(name);
                }
                if (unique.size() != ports.size())
                {
                    return false;
                }
                return std::all_of(ports.begin(), ports.end(), [&](const auto &port) {
                    return unique.contains(port.name);
                });
            };
            return named("inputPortName", source.inputPorts()) &&
                   named("outputPortName", source.outputPorts()) &&
                   named("inoutPortName", source.inoutPorts());
        }

        bool hasNamedPortMaps(wolvrix::lib::grh::AttrRange attrs,
                              const wolvrix::lib::grh::Graph &source)
        {
            const auto named = [&](std::string_view key, const auto &ports) {
                const auto names = getAttrStringsBorrowed(attrs, key);
                if (!names)
                {
                    return ports.empty();
                }
                if (names->size() != ports.size())
                {
                    return false;
                }
                std::unordered_set<std::string_view> unique;
                unique.reserve(names->size());
                for (const auto &name : *names)
                {
                    unique.insert(name);
                }
                if (unique.size() != ports.size())
                {
                    return false;
                }
                return std::all_of(ports.begin(), ports.end(), [&](const auto &port) {
                    return unique.contains(port.name);
                });
            };
            return named("inputPortName", source.inputPorts()) &&
                   named("outputPortName", source.outputPorts()) &&
                   named("inoutPortName", source.inoutPorts());
        }

        struct SourceFreezeStats
        {
            std::size_t eligible = 0;
            std::size_t frozen = 0;
            std::size_t alreadyFrozen = 0;
            std::size_t skippedPositional = 0;
            std::size_t failedFreeze = 0;
        };

        SourceFreezeStats freezeInlineSources(wolvrix::lib::grh::Design &design,
                                               const wolvrix::lib::grh::Graph &top)
        {
            struct References
            {
                bool inlineCandidate = false;
                bool named = true;
            };
            std::unordered_map<wolvrix::lib::grh::Graph *, References> sources;
            for (const auto &entry : design.graphs())
            {
                const auto &graph = entry.second;
                if (!graph) continue;
                for (const auto opId : graph->operations())
                {
                    const auto kind = graph->opKind(opId);
                    if (kind != wolvrix::lib::grh::OperationKind::kInstance &&
                        kind != wolvrix::lib::grh::OperationKind::kBlackbox) continue;
                    const auto attrs = graph->opAttrRange(opId);
                    const auto moduleName = getAttrStringBorrowed(attrs, "moduleName");
                    auto *source = moduleName ? design.findGraph(*moduleName) : nullptr;
                    if (!source || source == &top) continue;
                    auto &references = sources[source];
                    references.inlineCandidate |= kind == wolvrix::lib::grh::OperationKind::kInstance;
                    references.named = references.named && hasNamedPortMaps(attrs, *source);
                }
            }
            // Compact each read-only definition before retaining flatten IDs
            // or spans. Compact sorts ports, so every reference must use
            // complete named maps.
            SourceFreezeStats stats;
            for (const auto &[source, references] : sources)
            {
                if (!references.inlineCandidate) continue;
                if (!references.named)
                {
                    ++stats.skippedPositional;
                    continue;
                }
                ++stats.eligible;
                if (source->frozen())
                {
                    // Prefix workers can perform this same compact earlier.
                    // Consume its transient receipt so the legacy diagnostic
                    // still accounts for the work at this boundary once.
                    if (source->consumePreparedFlattenFreeze()) ++stats.frozen;
                    else ++stats.alreadyFrozen;
                    continue;
                }
                try
                {
                    source->freeze();
                    ++stats.frozen;
                }
                catch (const std::runtime_error &)
                {
                    // Invalid source structure must reach the existing inline
                    // diagnostics; a failed freeze leaves its builder intact.
                    ++stats.failedFreeze;
                }
            }
            return stats;
        }

        struct FlattenCapacityEstimate
        {
            std::size_t values = 0;
            std::size_t operations = 0;
            std::size_t symbols = 0;
        };

        void addCapacity(std::size_t &dst, std::size_t value)
        {
            const auto limit = std::numeric_limits<std::size_t>::max();
            dst = value > limit - dst ? limit : dst + value;
        }

        FlattenCapacityEstimate estimateFlattenCapacity(
            wolvrix::lib::grh::Design &design, const wolvrix::lib::grh::Graph &graph,
            std::unordered_map<const wolvrix::lib::grh::Graph *, FlattenCapacityEstimate> &memo,
            std::unordered_set<const wolvrix::lib::grh::Graph *> &stack)
        {
            if (const auto found = memo.find(&graph); found != memo.end())
            {
                return found->second;
            }
            if (!stack.insert(&graph).second)
            {
                // Recursive designs are rejected by the actual inline path;
                // avoid recursing here while still reserving this graph's own
                // storage.
                return {graph.values().size(), graph.operations().size(),
                        graph.values().size() + graph.operations().size()};
            }
            FlattenCapacityEstimate estimate{graph.values().size(), 0, graph.values().size()};
            for (const auto opId : graph.operations())
            {
                const auto kind = graph.opKind(opId);
                if (kind != wolvrix::lib::grh::OperationKind::kInstance)
                {
                    addCapacity(estimate.operations, 1);
                    addCapacity(estimate.symbols, 1);
                    continue;
                }
                addCapacity(estimate.symbols, 1);
                const auto attrs = graph.opAttrRange(opId);
                const auto moduleName = getAttrStringBorrowed(attrs, "moduleName");
                auto *child = moduleName ? design.findGraph(*moduleName) : nullptr;
                if (!child)
                {
                    addCapacity(estimate.operations, 1);
                    continue;
                }
                const auto childEstimate = estimateFlattenCapacity(design, *child, memo, stack);
                addCapacity(estimate.values, childEstimate.values);
                addCapacity(estimate.operations, childEstimate.operations);
                addCapacity(estimate.symbols, childEstimate.symbols);
            }
            stack.erase(&graph);
            memo.emplace(&graph, estimate);
            return estimate;
        }

        bool attrsEqual(const wolvrix::lib::grh::Operation &lhs,
                        const wolvrix::lib::grh::Operation &rhs)
        {
            const auto lhsAttrs = lhs.attrs();
            const auto rhsAttrs = rhs.attrs();
            if (lhsAttrs.size() != rhsAttrs.size())
            {
                return false;
            }
            for (const auto &attr : lhsAttrs)
            {
                auto rhsAttr = rhs.attr(attr.key);
                if (!rhsAttr || *rhsAttr != attr.value)
                {
                    return false;
                }
            }
            return true;
        }

        // DPI import equivalence is a signature check: provenance attrs such
        // as originModules differ per defining module and must not block the
        // merge of duplicate import declarations.
        bool dpiAttrsEqual(const wolvrix::lib::grh::Operation &lhs,
                           const wolvrix::lib::grh::Operation &rhs)
        {
            auto attrCount = [](const wolvrix::lib::grh::Operation &op)
            {
                std::size_t count = 0;
                for (const auto &attr : op.attrs())
                {
                    if (attr.key != kOriginModulesKey)
                    {
                        ++count;
                    }
                }
                return count;
            };
            if (attrCount(lhs) != attrCount(rhs))
            {
                return false;
            }
            for (const auto &attr : lhs.attrs())
            {
                if (attr.key == kOriginModulesKey)
                {
                    continue;
                }
                auto rhsAttr = rhs.attr(attr.key);
                if (!rhsAttr || *rhsAttr != attr.value)
                {
                    return false;
                }
            }
            return true;
        }

        std::string normalizeComponent(std::string_view text)
        {
            std::string normalized = wolvrix::lib::grh::Graph::normalizeComponent(text);
            if (normalized.empty())
            {
                normalized = "inst";
            }
            if (!normalized.empty() && normalized.front() >= '0' && normalized.front() <= '9')
            {
                normalized.insert(normalized.begin(), '_');
            }
            return normalized;
        }

        std::string makeHierName(std::string_view prefix, std::string_view base)
        {
            if (prefix.empty())
            {
                return std::string(base);
            }
            std::string out;
            out.reserve(prefix.size() + base.size() + 1);
            out.append(prefix);
            out.push_back('$');
            out.append(base);
            return out;
        }

        std::string uniqueInstanceComponent(InlineState &state,
                                             std::string_view parentPrefix,
                                             std::string base)
        {
            std::string key;
            key.reserve(parentPrefix.size() + base.size() + 1);
            key.append(parentPrefix);
            key.push_back('|');
            key.append(base);
            int &count = state.instanceCounts[key];
            count += 1;
            if (count == 1)
            {
                return base;
            }
            return base + "_" + std::to_string(count);
        }

        wolvrix::lib::grh::SymbolId internUniqueSymbol(wolvrix::lib::grh::Graph &graph, const std::string &base)
        {
            if (const auto symbol = graph.internSymbol(base); symbol.valid())
            {
                return symbol;
            }
            int suffix = 0;
            while (true)
            {
                const std::string candidate = base + "_" + std::to_string(++suffix);
                wolvrix::lib::grh::SymbolId sym = graph.internSymbol(candidate);
                if (sym.valid())
                {
                    return sym;
                }
            }
        }

        bool addResultChecked(InlineState &state,
                              wolvrix::lib::grh::Graph &target,
                              wolvrix::lib::grh::OperationId op,
                              wolvrix::lib::grh::ValueId value,
                              const wolvrix::lib::grh::Graph &source,
                              wolvrix::lib::grh::OperationId srcOp,
                              wolvrix::lib::grh::ValueId srcValue,
                              std::string_view prefix)
        {
            if (!value.valid())
            {
                state.opError(source, source.getOperation(srcOp), "Flatten mapping produced invalid result value");
                return false;
            }
            auto describePort = [&](const wolvrix::lib::grh::Graph &graph,
                                    wolvrix::lib::grh::ValueId valueId) -> std::string {
                if (!valueId.valid())
                {
                    return std::string();
                }
                for (const auto &port : graph.inputPorts())
                {
                    if (port.value == valueId)
                    {
                        return "input:" + port.name;
                    }
                }
                for (const auto &port : graph.outputPorts())
                {
                    if (port.value == valueId)
                    {
                        return "output:" + port.name;
                    }
                }
                for (const auto &port : graph.inoutPorts())
                {
                    if (port.in == valueId)
                    {
                        return "inout.in:" + port.name;
                    }
                    if (port.out == valueId)
                    {
                        return "inout.out:" + port.name;
                    }
                    if (port.oe == valueId)
                    {
                        return "inout.oe:" + port.name;
                    }
                }
                return std::string();
            };
            const wolvrix::lib::grh::OperationId definingOp = target.valueDef(value);
            if (definingOp.valid())
            {
                const wolvrix::lib::grh::Value dstValue = target.getValue(value);
                const wolvrix::lib::grh::Operation existing = target.getOperation(definingOp);
                std::string message = "Flatten result value already has defining op";
                message.append(" target_value=");
                message.append(target.valueDisplayName(dstValue.id()));
                message.append(" target_graph=");
                message.append(target.symbol());
                if (srcValue.valid())
                {
                    const wolvrix::lib::grh::Value srcVal = source.getValue(srcValue);
                    message.append(" source_value=");
                    message.append(source.valueDisplayName(srcVal.id()));
                    const std::string portDesc = describePort(source, srcValue);
                    if (!portDesc.empty())
                    {
                        message.append(" source_port=");
                        message.append(portDesc);
                    }
                }
                if (!prefix.empty())
                {
                    message.append(" instance_path=");
                    message.append(prefix);
                }
                message.append(" existing_def=");
                message.append(target.operationDisplayName(existing.id()));
                message.append(" existing_kind=");
                message.append(std::string(wolvrix::lib::grh::toString(existing.kind())));
                state.opError(source, source.getOperation(srcOp), std::move(message));
                return false;
            }
            target.addResult(op, value);
            return true;
        }

        bool inlineGraphContents(const wolvrix::lib::grh::Graph &source,
                                 wolvrix::lib::grh::Graph &target,
                                 const std::string &prefix,
                                 DenseValueMap valueMap,
                                 InlineState &state);

        wolvrix::lib::grh::ValueId mapInputPortType(wolvrix::lib::grh::Graph &target,
                                                     wolvrix::lib::grh::ValueId childValue,
                                                     wolvrix::lib::grh::ValueId targetValue,
                                                     const wolvrix::lib::grh::Graph &childGraph,
                                                     InlineState &state)
        {
            const auto expected = childGraph.valueLogicalType(childValue);
            const auto actual = target.valueLogicalType(targetValue);
            if (actual == expected)
            {
                return targetValue;
            }
            if (actual.width != expected.width || actual.kind != expected.kind || actual.domain != expected.domain)
            {
                state.graphError(childGraph, "Flatten input port requires equal width and compatible kind/domain");
                state.failed = true;
                return {};
            }
            auto &views = state.inputTypeViews[targetValue];
            for (const auto &[viewType, view] : views)
            {
                if (expected == viewType)
                {
                    return view;
                }
            }
            const wolvrix::lib::grh::ValueId view =
                target.createTemporaryValue(expected);
            state.plan.boundaryValue(view);
            const wolvrix::lib::grh::OperationId assign =
                target.createTemporaryOperation(wolvrix::lib::grh::OperationKind::kAssign);
            state.plan.boundaryOperation(assign);
            target.reserveOpOperandCapacity(assign, 1);
            target.reserveOpResultCapacity(assign, 1);
            target.addOperand(assign, targetValue);
            target.addResult(assign, view);
            const auto semantics = target.valueSemanticIds(targetValue);
            if (!semantics.empty()) target.setOpSemanticIds(assign, semantics);
            views.emplace_back(expected, view);
            state.changed = true;
            return view;
        }

        bool mapOutputPortType(wolvrix::lib::grh::Graph &target,
                               const wolvrix::lib::grh::Graph &child,
                               wolvrix::lib::grh::ValueId childValue,
                               wolvrix::lib::grh::ValueId targetValue,
                               DenseValueMap &portMap, InlineState &state)
        {
            const auto sourceType = child.valueLogicalType(childValue);
            const auto targetType = target.valueLogicalType(targetValue);
            if (sourceType.width != targetType.width || sourceType.kind != targetType.kind ||
                sourceType.domain != targetType.domain)
            {
                state.graphError(child, "Flatten output port requires equal width and compatible kind/domain");
                state.failed = true;
                return false;
            }
            auto found = portMap.get(childValue);
            if (!found.valid())
            {
                // A type bridge is a structural flattening temporary.  It has
                // no semantic reference attribute, so keep it out of the
                // SymbolTable until a textual downstream ABI explicitly
                // requests materialization.
                found = sourceType == targetType ? targetValue : target.createTemporaryValue(sourceType);
                if (sourceType != targetType) state.plan.boundaryValue(found);
                portMap.bindEquivalentPort(childValue, found);
            }
            if (found != targetValue)
            {
                if (target.valueDef(targetValue))
                {
                    state.graphError(child, "Flatten output alias already has a defining op");
                    state.failed = true;
                    return false;
                }
                const auto assign = target.createTemporaryOperation(wolvrix::lib::grh::OperationKind::kAssign);
                state.plan.boundaryOperation(assign);
                target.reserveOpOperandCapacity(assign, 1);
                target.reserveOpResultCapacity(assign, 1);
                target.addOperand(assign, found);
                target.addResult(assign, targetValue);
                const auto semantics = target.valueSemanticIds(targetValue);
                if (!semantics.empty()) target.setOpSemanticIds(assign, semantics);
                target.setOpSrcLoc(assign, makeTransformSrcLoc("hier-flatten", "output-view"));
                state.changed = true;
            }
            return true;
        }

        bool inlineInstanceInfo(const wolvrix::lib::grh::Graph &sourceGraph,
                                const InstanceInfo &info,
                                const DenseValueMap &valueMap,
                                wolvrix::lib::grh::Graph &target,
                                const std::string &prefix,
                                InlineState &state)
        {
            if (info.moduleName.empty())
            {
                state.graphError(sourceGraph, "Instance missing moduleName");
                return false;
            }
            wolvrix::lib::grh::Graph *child = state.design.findGraph(info.moduleName);
            if (!child)
            {
                state.graphError(sourceGraph, "Instance module not found: " + info.moduleName);
                return false;
            }

            const std::size_t inputCount = info.inputPortNames
                                               ? info.inputPortNames->size()
                                               : child->inputPorts().size();
            const std::size_t outputCount = info.outputPortNames
                                                ? info.outputPortNames->size()
                                                : child->outputPorts().size();
            const std::size_t inoutCount = info.inoutPortNames
                                               ? info.inoutPortNames->size()
                                               : child->inoutPorts().size();

            if (child->inputPorts().size() != inputCount ||
                child->outputPorts().size() != outputCount ||
                child->inoutPorts().size() != inoutCount)
            {
                std::string message = "Instance port list does not match module ports";
                message.append(" module=");
                message.append(info.moduleName);
                if (!info.instanceName.empty())
                {
                    message.append(" instance=");
                    message.append(info.instanceName);
                }
                message.append(" child_in=");
                message.append(std::to_string(child->inputPorts().size()));
                message.append(" attr_in=");
                message.append(std::to_string(inputCount));
                message.append(" child_out=");
                message.append(std::to_string(child->outputPorts().size()));
                message.append(" attr_out=");
                message.append(std::to_string(outputCount));
                message.append(" child_inout=");
                message.append(std::to_string(child->inoutPorts().size()));
                message.append(" attr_inout=");
                message.append(std::to_string(inoutCount));
                state.graphError(sourceGraph, std::move(message));
                return false;
            }

            const std::size_t expectedOperands = inputCount + inoutCount;
            const std::size_t expectedResults = outputCount + inoutCount * 2;
            if (info.operands.size() != expectedOperands ||
                info.results.size() != expectedResults)
            {
                state.graphError(sourceGraph, "Instance operand/result count mismatch during flatten");
                return false;
            }

            std::string baseName = !info.instanceName.empty() ? info.instanceName : info.moduleName;
            std::string component = normalizeComponent(baseName);
            component = uniqueInstanceComponent(state, prefix, component);
            const std::string childPrefix = makeHierName(prefix, component);

            auto mapValue = [&](wolvrix::lib::grh::ValueId src) -> std::optional<wolvrix::lib::grh::ValueId> {
                const auto mapped = valueMap.get(src);
                if (!mapped.valid())
                {
                    return std::nullopt;
                }
                return mapped;
            };

            // Declared child symbols are preserved in every symProtect mode by
            // inlineGraphContents; the mode only controls whether an undeclared
            // parent-side port value adopts the child hierarchical port name.
            const bool renamePorts =
                state.symProtect == HierFlattenOptions::SymProtectMode::All ||
                state.symProtect == HierFlattenOptions::SymProtectMode::Hierarchy;

            auto maybeRenameMappedValue = [&](wolvrix::lib::grh::ValueId childValue,
                                              wolvrix::lib::grh::ValueId targetValue,
                                              std::string_view baseName) {
                if (!renamePorts)
                {
                    return;
                }
                if (!childValue.valid() || !targetValue.valid())
                {
                    return;
                }
                std::string base;
                if (!baseName.empty())
                {
                    base = std::string(baseName);
                }
                else
                {
                    base = std::string(child->symbolText(child->valueSymbol(childValue)));
                }
                if (base.empty())
                {
                    return;
                }
                const auto targetSymbol = target.valueSymbol(targetValue);
                const auto targetName = target.symbolText(targetSymbol);
                if (target.isDeclaredSymbol(targetSymbol) &&
                    !targetName.starts_with("_val_"))
                {
                    return;
                }
                const std::string desiredName = makeHierName(childPrefix, base);
                if (desiredName.empty() || targetName == desiredName)
                {
                    return;
                }
                auto renamedIt = state.renamedValues.find(targetValue);
                if (renamedIt != state.renamedValues.end())
                {
                    if (renamedIt->second != desiredName)
                    {
                        state.graphError(sourceGraph,
                                         "Flatten port maps share value with different names: " +
                                             renamedIt->second + " vs " + desiredName);
                        state.failed = true;
                    }
                    return;
                }
                wolvrix::lib::grh::SymbolId sym = internUniqueSymbol(target, desiredName);
                target.setValueSymbol(targetValue, sym);
                target.addDeclaredSymbol(sym);
                state.renamedValues.emplace(targetValue, std::string(target.symbolText(sym)));
                state.changed = true;
            };

            DenseValueMap childPortMap(*child);
            if (state.plan.enabled)
            {
                const auto instance = state.plan.nextInstance++;
                if (instance >= state.plan.instances.size() || state.plan.instances[instance].source != child)
                    throw std::logic_error("Flatten global instance order differs from legacy traversal");
                childPortMap.globalPorts = &state.plan.ports;
                childPortMap.globalBase = state.plan.instances[instance].nodeBase;
                childPortMap.plannedInstance = instance;
            }

            const auto childInputs = child->inputPorts();
            std::unordered_map<std::string_view, wolvrix::lib::grh::ValueId> childInputByName;
            if (info.inputPortNames)
            {
                childInputByName.reserve(childInputs.size());
                for (const auto &port : childInputs)
                {
                    childInputByName.emplace(port.name, port.value);
                }
            }
            for (std::size_t i = 0; i < inputCount; ++i)
            {
                wolvrix::lib::grh::ValueId childValue = wolvrix::lib::grh::ValueId::invalid();
                std::string_view portName;
                if (info.inputPortNames)
                {
                    portName = (*info.inputPortNames)[i];
                    auto it = childInputByName.find(portName);
                    if (it == childInputByName.end())
                    {
                        state.graphError(sourceGraph, "Flatten missing input port: " + std::string(portName));
                        return false;
                    }
                    childValue = it->second;
                }
                else
                {
                    portName = childInputs[i].name;
                    childValue = childInputs[i].value;
                }
                auto targetValue = mapValue(info.operands[i]);
                if (!targetValue)
                {
                    state.graphError(sourceGraph, "Flatten missing mapping for instance input operand");
                    return false;
                }
                const auto mappedInput = mapInputPortType(target, childValue, *targetValue,
                                                            *child, state);
                if (!mappedInput) return false;
                maybeRenameMappedValue(childValue, *targetValue, portName);
                if (!childPortMap.contains(childValue))
                {
                    childPortMap.bindEquivalentPort(childValue, mappedInput);
                }
            }

            const auto childOutputs = child->outputPorts();
            std::unordered_map<std::string_view, wolvrix::lib::grh::ValueId> childOutputByName;
            if (info.outputPortNames)
            {
                childOutputByName.reserve(childOutputs.size());
                for (const auto &port : childOutputs)
                {
                    childOutputByName.emplace(port.name, port.value);
                }
            }
            for (std::size_t i = 0; i < outputCount; ++i)
            {
                wolvrix::lib::grh::ValueId childValue = wolvrix::lib::grh::ValueId::invalid();
                std::string_view portName;
                if (info.outputPortNames)
                {
                    portName = (*info.outputPortNames)[i];
                    auto it = childOutputByName.find(portName);
                    if (it == childOutputByName.end())
                    {
                        state.graphError(sourceGraph, "Flatten missing output port: " + std::string(portName));
                        return false;
                    }
                    childValue = it->second;
                }
                else
                {
                    portName = childOutputs[i].name;
                    childValue = childOutputs[i].value;
                }
                auto targetValue = mapValue(info.results[i]);
                if (!targetValue)
                {
                    state.graphError(sourceGraph, "Flatten missing mapping for instance output result");
                    return false;
                }
                maybeRenameMappedValue(childValue, *targetValue, portName);
                if (!mapOutputPortType(target, *child, childValue, *targetValue, childPortMap, state))
                    return false;
            }

            const auto childInouts = child->inoutPorts();
            std::unordered_map<std::string_view, std::size_t> childInoutByName;
            if (info.inoutPortNames)
            {
                childInoutByName.reserve(childInouts.size());
                for (std::size_t i = 0; i < childInouts.size(); ++i)
                {
                    childInoutByName.emplace(childInouts[i].name, i);
                }
            }
            for (std::size_t i = 0; i < inoutCount; ++i)
            {
                std::size_t childIndex = i;
                if (info.inoutPortNames)
                {
                    const std::string_view portName = (*info.inoutPortNames)[i];
                    auto it = childInoutByName.find(portName);
                    if (it == childInoutByName.end())
                    {
                        state.graphError(sourceGraph, "Flatten missing inout port: " + std::string(portName));
                        return false;
                    }
                    childIndex = it->second;
                }
                const std::size_t inIndex = inputCount + i;
                const std::size_t outIndex = outputCount + i;
                const std::size_t oeIndex = outputCount + inoutCount + i;
                auto inValue = mapValue(info.operands[inIndex]);
                auto outValue = mapValue(info.results[outIndex]);
                auto oeValue = mapValue(info.results[oeIndex]);
                if (!outValue || !oeValue || !inValue)
                {
                    state.graphError(sourceGraph, "Flatten missing mapping for instance inout port");
                    return false;
                }
                const auto &childInout = childInouts[childIndex];
                const auto mappedInput = mapInputPortType(target, childInout.in, *inValue,
                                                            *child, state);
                if (!mappedInput) return false;
                maybeRenameMappedValue(childInout.out, *outValue, std::string_view());
                maybeRenameMappedValue(childInout.oe, *oeValue, std::string_view());
                maybeRenameMappedValue(childInout.in, *inValue, std::string_view());
                if (!mapOutputPortType(target, *child, childInout.out, *outValue, childPortMap, state) ||
                    !mapOutputPortType(target, *child, childInout.oe, *oeValue, childPortMap, state))
                    return false;
                if (!childPortMap.contains(childInout.in))
                {
                    childPortMap.bindEquivalentPort(childInout.in, mappedInput);
                }
            }
            if (state.failed)
            {
                return false;
            }

            const auto previousInstanceSemantic = state.currentInstanceSemantic;
            if (info.semantic) state.currentInstanceSemantic = info.semantic;
            state.nodeInstancePath.push_back(component);
            const bool success = inlineGraphContents(*child, target, childPrefix, std::move(childPortMap), state);
            state.nodeInstancePath.pop_back();
            state.currentInstanceSemantic = previousInstanceSemantic;
            return success;
        }

        InstanceInfo buildInstanceInfo(const wolvrix::lib::grh::Operation &op)
        {
            InstanceInfo info;
            info.moduleName = getAttrString(op, "moduleName").value_or(std::string());
            info.instanceName = getAttrString(op, "instanceName").value_or(std::string());
            info.operands.assign(op.operands().begin(), op.operands().end());
            info.results.assign(op.results().begin(), op.results().end());
            info.inputPortNames = getAttrStrings(op, "inputPortName");
            info.outputPortNames = getAttrStrings(op, "outputPortName");
            info.inoutPortNames = getAttrStrings(op, "inoutPortName");
            return info;
        }

        InstanceInfo buildInstanceInfo(const wolvrix::lib::grh::Graph &graph,
                                       wolvrix::lib::grh::OperationId opId)
        {
            InstanceInfo info;
            const auto attrs = graph.opAttrRange(opId);
            info.moduleName = getAttrStringBorrowed(attrs, "moduleName").value_or(std::string_view());
            info.instanceName = getAttrStringBorrowed(attrs, "instanceName").value_or(std::string_view());
            const auto semantics = graph.opSemanticIds(opId);
            if (!semantics.empty()) info.semantic = semantics.front();
            const auto operands = graph.opOperands(opId);
            const auto results = graph.opResults(opId);
            info.operands.assign(operands.begin(), operands.end());
            info.results.assign(results.begin(), results.end());
            if (const auto names = getAttrStringsBorrowed(attrs, "inputPortName"))
            {
                info.inputPortNames = std::vector<std::string>(names->begin(), names->end());
            }
            if (const auto names = getAttrStringsBorrowed(attrs, "outputPortName"))
            {
                info.outputPortNames = std::vector<std::string>(names->begin(), names->end());
            }
            if (const auto names = getAttrStringsBorrowed(attrs, "inoutPortName"))
            {
                info.inoutPortNames = std::vector<std::string>(names->begin(), names->end());
            }
            return info;
        }

        GlobalInlinePlan planGlobalInline(wolvrix::lib::grh::Design &design,
                                         const wolvrix::lib::grh::Graph &top,
                                         std::span<const wolvrix::lib::grh::OperationId> topInstances)
        {
            GlobalInlinePlan plan;
            if (topInstances.empty()) return plan;
            const auto rowCount = [](const wolvrix::lib::grh::Graph &source) {
                std::size_t count = 1;
                for (const auto value : source.values())
                    count = std::max(count, static_cast<std::size_t>(value.index) + 1);
                return count;
            };
            std::size_t nodes = rowCount(top);
            std::unordered_set<const wolvrix::lib::grh::Graph *> stack{&top};
            std::unordered_set<const wolvrix::lib::grh::Graph *> validated;
            std::function<bool(const wolvrix::lib::grh::Graph &)> expand;
            expand = [&](const wolvrix::lib::grh::Graph &source) {
                if (!stack.insert(&source).second) return false;
                const auto instance = plan.instances.size();
                const auto count = rowCount(source);
                if (validated.insert(&source).second)
                {
                    std::vector<uint8_t> live(count, 0);
                    for (const auto value : source.values()) live[value.index] = 1;
                    const auto validValue = [&](wolvrix::lib::grh::ValueId value) {
                        return value.valid() && value.graph == source.id() && value.generation == 0 &&
                            value.index < live.size() && live[value.index];
                    };
                    for (const auto &port : source.inputPorts()) if (!validValue(port.value)) return false;
                    for (const auto &port : source.outputPorts()) if (!validValue(port.value)) return false;
                    for (const auto &port : source.inoutPorts())
                        if (!validValue(port.in) || !validValue(port.out) || !validValue(port.oe)) return false;
                    for (const auto op : source.operations())
                    {
                        for (const auto value : source.opOperands(op)) if (!validValue(value)) return false;
                        for (const auto value : source.opResults(op)) if (!validValue(value)) return false;
                        for (const auto attr : source.opAttrRange(op))
                        {
                            if (!isOpSymbolReferenceAttr(attr.key())) continue;
                            if (const auto text = attr.value().get<std::string>();
                                text && !source.findOperation(*text).valid()) return false;
                        }
                    }
                    if (!wolvrix::lib::ingest::node::scanAttributeErrors(source).empty()) return false;
                }
                if (count > std::numeric_limits<std::size_t>::max() - nodes)
                    throw std::length_error("Flatten global value range capacity");
                plan.instances.push_back({&source, nodes, planOperationRuns(source)});
                ++plan.definitionUses[&source];
                nodes += count;
                plan.events.push_back({GlobalInlinePlan::EventKind::EnterValues, instance, 0});
                // Copy the small run descriptors: recursive expansion grows
                // instances and must not invalidate the parent's iterator.
                const auto runs = plan.instances[instance].runs;
                for (std::size_t run = 0; run < runs.size(); ++run)
                {
                    const auto &entry = runs[run];
                    plan.events.push_back({entry.boundary ? GlobalInlinePlan::EventKind::Boundary :
                        GlobalInlinePlan::EventKind::OpRun, instance, run});
                    if (!entry.boundary || source.opKind(entry.operations.front()) !=
                        wolvrix::lib::grh::OperationKind::kInstance) continue;
                    const auto name = getAttrStringBorrowed(source.opAttrRange(entry.operations.front()), "moduleName");
                    const auto *child = name ? design.findGraph(*name) : nullptr;
                    if (!child || !expand(*child)) return false;
                }
                plan.events.push_back({GlobalInlinePlan::EventKind::Exit, instance, 0});
                stack.erase(&source);
                return true;
            };
            for (std::size_t index = 0; index < topInstances.size(); ++index)
            {
                const auto op = topInstances[index];
                plan.events.push_back({GlobalInlinePlan::EventKind::Boundary, GlobalPortUnionFind::invalid, index});
                const auto name = getAttrStringBorrowed(top.opAttrRange(op), "moduleName");
                const auto *child = name ? design.findGraph(*name) : nullptr;
                // Invalid structures retain the old mutation and first-error
                // route; planning itself must not publish diagnostics.
                if (!child || !expand(*child)) return {};
            }
            plan.ports.nodes.resize(nodes);
            plan.enabled = true;
            return plan;
        }

        // Propagate the inlined child graph's generate groups into the target:
        // members are remapped to the cloned/renamed symbols, the group scope
        // gains the instance path prefix (e.g. "gen_loop" -> "u_inst$gen_loop"),
        // and the bare declaration name is kept unchanged. Members whose symbols
        // were dropped during inlining (e.g. child port names superseded by the
        // parent-side declared name) are removed; groups left empty are skipped.
        bool propagateGenerateGroups(const wolvrix::lib::grh::Graph &source,
                                     wolvrix::lib::grh::Graph &target,
                                     const std::string &prefix,
                                     const SymbolRenameMap &valueRename,
                                     const SymbolRenameMap &opRename,
                                     InlineState &state)
        {
            auto lookupOrIntern = [&](std::string_view text) -> wolvrix::lib::grh::SymbolId {
                wolvrix::lib::grh::SymbolId sym = target.lookupSymbol(text);
                if (!sym.valid())
                {
                    sym = target.internSymbol(text);
                }
                return sym;
            };
            for (const auto &srcGroup : source.generateGroups())
            {
                const std::string_view srcScope = source.symbolText(srcGroup.scope);
                const std::string_view srcName = source.symbolText(srcGroup.name);
                if (srcScope.empty() || srcName.empty())
                {
                    continue;
                }
                std::vector<wolvrix::lib::grh::SymbolId> newMembers;
                newMembers.reserve(srcGroup.symbols.size());
                for (const wolvrix::lib::grh::SymbolId srcSym : srcGroup.symbols)
                {
                    if (!srcSym.valid())
                    {
                        continue;
                    }
                    if (source.symbolText(srcSym).empty())
                    {
                        continue;
                    }
                    wolvrix::lib::grh::SymbolId dstSym;
                    if (auto it = valueRename.find(srcSym.value); it != valueRename.end())
                    {
                        dstSym = it->second;
                    }
                    else if (auto it = opRename.find(srcSym.value); it != opRename.end())
                    {
                        dstSym = it->second;
                    }
                    else
                    {
                        // Symbol was dropped while inlining (port mapping, etc.).
                        continue;
                    }
                    // Entries are created only from successfully interned
                    // target symbols, whose IDs survive later value renames.
                    newMembers.push_back(dstSym);
                }
                if (newMembers.empty())
                {
                    continue;
                }
                const std::string newScope = makeHierName(prefix, srcScope);
                const wolvrix::lib::grh::SymbolId scopeSym = lookupOrIntern(newScope);
                const wolvrix::lib::grh::SymbolId nameSym = lookupOrIntern(srcName);
                if (!scopeSym.valid() || !nameSym.valid())
                {
                    state.graphError(source,
                                     "Flatten generate group scope/name collision: " + newScope);
                    state.failed = true;
                    return false;
                }
                const std::size_t dstGroup = target.addGenerateGroup(scopeSym, nameSym);
                for (const wolvrix::lib::grh::SymbolId sym : newMembers)
                {
                    target.addGenerateGroupSymbol(dstGroup, sym);
                }
            }
            return true;
        }

        bool inlineGraphContents(const wolvrix::lib::grh::Graph &source,
                                 wolvrix::lib::grh::Graph &target,
                                 const std::string &prefix,
                                 DenseValueMap valueMap,
                                 InlineState &state)
        {
            if (state.failed)
            {
                return false;
            }
            if (state.stack.find(&source) != state.stack.end())
            {
                state.graphError(source, "Recursive instance detected during hier-flatten");
                state.failed = true;
                return false;
            }
            state.stack.insert(&source);
            struct SemanticParentGuard {
                InlineState& state;
                wolvrix::lib::grh::SemanticId parent;
                ~SemanticParentGuard() { state.currentSemanticParent = parent; }
            } semanticParentGuard{state, state.currentSemanticParent};
            wolvrix::lib::grh::SemanticId copiedSemanticRoot = 0;
            if (source.semanticTree())
            {
                if (!target.semanticTree())
                {
                    state.graphError(source, "Flatten source has a SemanticTree but target does not");
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
                if (state.nodeInstancePath.empty())
                {
                    state.graphError(source, "Flatten instance path is empty during SemanticTree copy");
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
                const auto semanticStarted = state.profile ? FlattenClock::now() : FlattenClock::time_point{};
                const auto nameBytesBefore = target.semanticTree()->nameBytes();
                copiedSemanticRoot = target.appendSemanticTree(source, state.currentSemanticParent,
                                                               state.nodeInstancePath.back());
                if (state.profile)
                {
                    state.metrics.semanticCopy += FlattenClock::now() - semanticStarted;
                    state.metrics.semanticNodesCopied += source.semanticTree()->size();
                    state.metrics.semanticNameBytesCopied += target.semanticTree()->nameBytes() - nameBytesBefore;
                }
                if (copiedSemanticRoot) state.currentSemanticParent = copiedSemanticRoot;
            }
            const auto fallbackSemantic = state.currentInstanceSemantic;
            const auto plannedInstance = valueMap.plannedInstance;
            const auto enterEvent = state.plan.consume(GlobalInlinePlan::EventKind::EnterValues, plannedInstance);
            if (state.plan.enabled)
            {
                auto &instance = state.plan.instances[plannedInstance];
                instance.prefix = prefix;
            }
            ++state.metrics.sourceCalls;
            const auto sourceGraphId = source.id();
            const uint64_t sourceKey = (static_cast<uint64_t>(sourceGraphId.index) << 32) |
                sourceGraphId.generation;
            auto& srcLocImport = state.srcLocImports[sourceKey];
            auto& blockImport = state.blockImports[sourceKey];
            target.prepareBlockImport(source, blockImport, srcLocImport);

            const auto remapSemantics = [&](std::span<const wolvrix::lib::grh::SemanticId> sourceIds,
                                            slang::SmallVector<wolvrix::lib::grh::SemanticId, 4>& targetIds) {
                targetIds.clear();
                for (const auto sourceId : sourceIds)
                {
                    if (!copiedSemanticRoot || !source.semanticTree()->contains(sourceId))
                        throw std::invalid_argument("Flatten encountered an invalid source SemanticId");
                    const auto targetId = copiedSemanticRoot + sourceId - 1;
                    if (!target.semanticTree()->contains(targetId))
                        throw std::invalid_argument("Flatten SemanticId relocation left the copied tree");
                    targetIds.push_back(targetId);
                }
            };
            slang::SmallVector<wolvrix::lib::grh::SemanticId, 4> mappedSemantics;
            const auto copyOperationSemantics = [&](wolvrix::lib::grh::OperationId sourceOp,
                                                    wolvrix::lib::grh::OperationId targetOp) {
                const auto sourceIds = source.opSemanticIds(sourceOp);
                if (sourceIds.empty())
                {
                    if (!source.semanticTree() && fallbackSemantic)
                    {
                        target.setOpSemanticIds(targetOp, std::span<const wolvrix::lib::grh::SemanticId>(&fallbackSemantic, 1));
                        ++state.metrics.annotatedOperations;
                    }
                    return;
                }
                ++state.metrics.annotatedOperations;
                remapSemantics(sourceIds, mappedSemantics);
                target.setOpSemanticIds(targetOp,
                    std::span<const wolvrix::lib::grh::SemanticId>(mappedSemantics.data(), mappedSemantics.size()));
            };
            const auto mergeValueSemantics = [&](wolvrix::lib::grh::ValueId sourceValue,
                                                 wolvrix::lib::grh::ValueId targetValue) {
                const auto sourceIds = source.valueSemanticIds(sourceValue);
                if (sourceIds.empty()) return;
                remapSemantics(sourceIds, mappedSemantics);
                slang::SmallVector<wolvrix::lib::grh::SemanticId, 4> merged;
                const auto existing = target.valueSemanticIds(targetValue);
                merged.append(existing.begin(), existing.end());
                for (const auto id : mappedSemantics)
                    if (std::find(merged.begin(), merged.end(), id) == merged.end()) merged.push_back(id);
                if (merged.size() != existing.size())
                    target.setValueSemanticIds(targetValue,
                        std::span<const wolvrix::lib::grh::SemanticId>(merged.data(), merged.size()));
            };

            std::unordered_set<uint32_t> declaredSet;
            declaredSet.reserve(source.declaredSymbols().size());
            for (const auto sym : source.declaredSymbols())
            {
                if (sym.valid())
                {
                    declaredSet.insert(sym.value);
                }
            }
            auto isDeclared = [&](wolvrix::lib::grh::SymbolId sym) -> bool {
                return sym.valid() && declaredSet.find(sym.value) != declaredSet.end();
            };
            // Declared symbols are always preserved under their hierarchical
            // name and re-registered as declared, independent of
            // HierFlattenOptions::symProtect (the mode now only gates
            // parent-side port renaming, see renamePorts).
            auto shouldProtectValue = [&](wolvrix::lib::grh::SymbolId symbol) -> bool {
                return isDeclared(symbol);
            };
            auto shouldProtectOp = [&](wolvrix::lib::grh::SymbolId symbol) -> bool {
                return isDeclared(symbol);
            };

            // The caller no longer needs its completed port map. Reuse its
            // dense storage rather than rescanning the source and allocating,
            // clearing, then seeding a second vector for every instance.
            if (valueMap.graph != source.id())
            {
                throw std::invalid_argument("Flatten port mapping source graph mismatch");
            }
            auto requireMappedPort = [&](wolvrix::lib::grh::ValueId valueId, std::string_view role) -> bool {
                if (!valueMap.contains(valueId))
                {
                    state.graphError(source, "Flatten missing mapping for port value (" + std::string(role) + ")");
                    return false;
                }
                return true;
            };
            for (const auto &port : source.inputPorts())
            {
                if (!requireMappedPort(port.value, "input"))
                {
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
            }
            for (const auto &port : source.outputPorts())
            {
                if (!requireMappedPort(port.value, "output"))
                {
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
            }
            for (const auto &port : source.inoutPorts())
            {
                if (!requireMappedPort(port.in, "inout.in") ||
                    !requireMappedPort(port.out, "inout.out") ||
                    !requireMappedPort(port.oe, "inout.oe"))
                {
                    state.failed = true;
                    state.stack.erase(&source);
                    return false;
                }
            }

            // Only generate-group propagation consumes value names. Most
            // FIRRTL definitions have no groups, so avoid retaining a second
            // pair of long strings for every cloned intermediate value.
            const bool trackValueRename = !source.generateGroups().empty();
            SymbolRenameMap valueRename;
            const auto valuesStarted = state.profile ? FlattenClock::now() : FlattenClock::time_point{};
            state.metrics.valuesVisited += source.values().size();
            std::vector<wolvrix::lib::grh::ValueId> sourceValues;
            std::vector<wolvrix::lib::grh::ValueId> semanticPorts;
            std::vector<wolvrix::lib::grh::SymbolId> valueSymbols;
            sourceValues.reserve(source.values().size());
            valueSymbols.reserve(source.values().size());
            for (const auto valueId : source.values())
            {
                if (!valueId.valid())
                {
                    continue;
                }
                if (valueMap.contains(valueId))
                {
                    semanticPorts.push_back(valueId);
                    continue;
                }
                const wolvrix::lib::grh::SymbolId sourceSym = source.valueSymbol(valueId);
                const std::string_view sourceName = source.symbolText(sourceSym);
                wolvrix::lib::grh::SymbolId newSym = wolvrix::lib::grh::SymbolId::invalid();
                if (shouldProtectValue(sourceSym))
                {
                    if (!sourceName.empty())
                    {
                        const std::string hierName = makeHierName(prefix, sourceName);
                        newSym = internUniqueSymbol(target, hierName);
                        target.addDeclaredSymbol(newSym);
                    }
                }
                // Unprotected values are implementation temporaries.  Keep
                // their source identity only in the dense relocation map;
                // names are materialized at a downstream boundary if needed.
                sourceValues.push_back(valueId);
                valueSymbols.push_back(newSym);
                if (trackValueRename && !sourceName.empty())
                {
                    valueRename.emplace(sourceSym.value, newSym);
                }
            }
            std::vector<wolvrix::lib::grh::ValueId> newValues(sourceValues.size());
            const bool retainColumns = state.plan.enabled && state.plan.definitionUses.at(&source) == 1;
            target.appendValueBlock(source, sourceValues, valueSymbols, newValues, srcLocImport,
                state.plan.enabled ? 1 : state.numThreads, retainColumns);
            target.remapValueBlockSemantics(source, sourceValues, newValues, copiedSemanticRoot);
            if (state.plan.enabled)
            {
                auto &event = state.plan.events[enterEvent];
                event.count = newValues.size();
                if (!newValues.empty()) event.firstValue = newValues.front();
            }
            for (std::size_t index = 0; index < sourceValues.size(); ++index)
                valueMap.set(sourceValues[index], newValues[index]);
            for (const auto sourceValue : semanticPorts)
            {
                const auto targetValue = valueMap.get(sourceValue);
                if (targetValue.valid()) mergeValueSemantics(sourceValue, targetValue);
            }
            state.metrics.valuesCloned += sourceValues.size();
            state.changed |= !sourceValues.empty();
            if (state.profile)
            {
                state.metrics.values += FlattenClock::now() - valuesStarted;
            }

            // The name map serves only string-valued symbol references and
            // generate groups. Discover that set before cloning so forward
            // references still enter pendingAttrs and resolve exactly as before.
            // Use source SymbolIds for the filter; unbound/unknown names remain
            // unresolved and retain the existing pending-reference diagnostic.
            std::unordered_set<uint32_t> uncachedOpSymbols;
            const auto symbolsStarted = state.profile ? FlattenClock::now() : FlattenClock::time_point{};
            const auto &neededOpSymbols = [&]() -> const std::unordered_set<uint32_t> & {
                if (!source.frozen())
                {
                    uncachedOpSymbols = collectNeededOpSymbols(source);
                    return uncachedOpSymbols;
                }
                // Frozen definitions cannot change while their instances are
                // cloned. The reference-name discovery is independent of the
                // instance path; retain it once per definition, not per clone.
                auto [it, inserted] = state.neededOpSymbols.try_emplace(&source);
                if (inserted)
                {
                    it->second = collectNeededOpSymbols(source);
                }
                return it->second;
            }();
            if (state.profile)
            {
                state.metrics.symbols += FlattenClock::now() - symbolsStarted;
            }

            SymbolRenameMap opRename;
            opRename.reserve(neededOpSymbols.size());
            std::vector<PendingAttr> pendingAttrs;

            auto rewriteAttr = [&](wolvrix::lib::grh::OperationId newOp,
                                   std::string_view key,
                                   wolvrix::lib::grh::AttrValueView value) {
                if (const auto strValue = value.get<std::string>();
                    strValue && isOpSymbolReferenceAttr(key))
                {
                    const auto sourceSymbol = source.lookupSymbol(*strValue);
                    auto it = opRename.find(sourceSymbol.value);
                    if (it != opRename.end())
                    {
                        target.setAttr(newOp, key, std::string(target.symbolText(it->second)));
                        return;
                    }
                    pendingAttrs.push_back(PendingAttr{newOp, std::string(key), std::string(*strValue), sourceSymbol});
                }
                target.setAttrView(newOp, key, value);
            };

            const auto planOperationSymbol = [&](wolvrix::lib::grh::OperationId opId) {
                const auto sourceSym = source.operationSymbol(opId);
                const auto sourceName = source.symbolText(sourceSym);
                const auto kind = source.opKind(opId);
                const bool stateOrExternal =
                    kind == wolvrix::lib::grh::OperationKind::kRegister ||
                    kind == wolvrix::lib::grh::OperationKind::kLatch ||
                    kind == wolvrix::lib::grh::OperationKind::kSystemTask ||
                    kind == wolvrix::lib::grh::OperationKind::kSystemFunction ||
                    kind == wolvrix::lib::grh::OperationKind::kBlackbox ||
                    neededOpSymbols.contains(sourceSym.value);
                auto symbol = wolvrix::lib::grh::SymbolId::invalid();
                if ((shouldProtectOp(sourceSym) || stateOrExternal) && !sourceName.empty())
                {
                    symbol = internUniqueSymbol(target, makeHierName(prefix, sourceName));
                    if (shouldProtectOp(sourceSym)) target.addDeclaredSymbol(symbol);
                }
                return symbol;
            };

            const auto operationsStarted = state.profile ? FlattenClock::now() : FlattenClock::time_point{};
            state.metrics.operationsVisited += source.operations().size();
            const auto fallbackRuns = state.plan.enabled ? std::vector<OperationRun>{} : planOperationRuns(source);
            const auto &runs = state.plan.enabled ? state.plan.instances[plannedInstance].runs : fallbackRuns;
            for (std::size_t plannedRun = 0; plannedRun < runs.size(); ++plannedRun)
            {
                if (state.failed) break;
                const auto &run = runs[plannedRun];
                const auto runEvent = state.plan.consume(run.boundary ? GlobalInlinePlan::EventKind::Boundary :
                    GlobalInlinePlan::EventKind::OpRun, plannedInstance, plannedRun);
                std::vector<wolvrix::lib::grh::OperationId> runOutput;
                std::vector<wolvrix::lib::grh::SymbolId> runSymbols;
                std::vector<uint32_t> operandOffsets{0}, resultOffsets{0};
                std::vector<wolvrix::lib::grh::ValueId> operands, results;
                std::vector<std::size_t> operandNodes;
                bool bulk = !run.boundary;
                slang::flat_hash_set<uint32_t, std::hash<uint32_t>> definitions;
                if (bulk)
                {
                    // Validate the whole ordinary interior before committing
                    // it. Invalid inputs retain the old operation-at-a-time
                    // route and its exact first-error diagnostic.
                    for (const auto sourceOp : run.operations)
                    {
                        for (const auto value : source.opOperands(sourceOp))
                        {
                            if (state.plan.enabled)
                            {
                                const auto node = valueMap.node(value);
                                bulk &= node != GlobalPortUnionFind::invalid;
                                operandNodes.push_back(node);
                            }
                            else
                            {
                                const auto mapped = valueMap.get(value);
                                bulk &= mapped.valid();
                                operands.push_back(mapped);
                            }
                        }
                        operandOffsets.push_back(static_cast<uint32_t>(state.plan.enabled ? operandNodes.size() : operands.size()));
                        for (const auto value : source.opResults(sourceOp))
                        {
                            const auto mapped = valueMap.get(value);
                            if (!mapped.valid() || target.valueDef(mapped).valid() ||
                                !definitions.insert(mapped.index).second)
                                bulk = false;
                            results.push_back(mapped);
                        }
                        resultOffsets.push_back(static_cast<uint32_t>(results.size()));
                    }
                }
                if (bulk)
                {
                    runSymbols.reserve(run.operations.size());
                    for (const auto sourceOp : run.operations) runSymbols.push_back(planOperationSymbol(sourceOp));
                    runOutput.resize(run.operations.size());
                    target.appendOperationBlock(source, run.operations, runSymbols, runOutput, srcLocImport,
                        state.plan.enabled ? 1 : state.numThreads, retainColumns);
                    if (!source.semanticTree())
                    {
                        if (fallbackSemantic)
                        {
                            for (const auto targetOp : runOutput)
                                target.setOpSemanticIds(targetOp,
                                    std::span<const wolvrix::lib::grh::SemanticId>(&fallbackSemantic, 1));
                            state.metrics.annotatedOperations += runOutput.size();
                        }
                    }
                    else
                    {
                        state.metrics.annotatedOperations += target.remapOperationBlockSemantics(
                            source, run.operations, runOutput, copiedSemanticRoot);
                    }
                    if (state.plan.enabled)
                    {
                        auto &event = state.plan.events[runEvent];
                        event.count = runOutput.size();
                        if (!runOutput.empty()) event.firstOperation = runOutput.front();
                        target.setOperationBlockResults(runOutput, resultOffsets, results);
                        state.plan.operands.push_back({&source, run.operations, plannedInstance, runOutput,
                            std::move(operandOffsets), std::move(operandNodes)});
                    }
                    else
                    {
                        target.setOperationBlockEdges(runOutput, operandOffsets, operands, resultOffsets, results);
                    }
                    if (!state.plan.enabled)
                        target.appendOperationBlockAttributes(source, run.operations, runOutput, blockImport);
                }
                for (std::size_t runIndex = 0; runIndex < run.operations.size(); ++runIndex)
                {
                    const auto opId = run.operations[runIndex];
                    if (!opId.valid())
                    {
                        continue;
                    }
                    const wolvrix::lib::grh::OperationKind kind = source.opKind(opId);
                    if (kind == wolvrix::lib::grh::OperationKind::kInstance)
                    {
                        ++state.metrics.instanceCalls;
                        ++state.metrics.instanceOperations;
                        InstanceInfo info = buildInstanceInfo(source, opId);
                        if (info.semantic)
                        {
                            if (!copiedSemanticRoot || !source.semanticTree()->contains(info.semantic))
                                throw std::invalid_argument("Flatten encountered an invalid instance SemanticId");
                            info.semantic = copiedSemanticRoot + info.semantic - 1;
                        }
                        if (state.profile)
                        {
                            const auto started = FlattenClock::now();
                            if (!inlineInstanceInfo(source, info, valueMap, target, prefix, state))
                            {
                                state.failed = true;
                                break;
                            }
                            state.metrics.recursiveInstances += FlattenClock::now() - started;
                        }
                        else if (!inlineInstanceInfo(source, info, valueMap, target, prefix, state))
                        {
                            state.failed = true;
                            break;
                        }
                        state.changed = true;
                        continue;
                    }

                    if (kind == wolvrix::lib::grh::OperationKind::kDpicImport)
                    {
                        const wolvrix::lib::grh::Operation op = source.getOperation(opId);
                        const std::string opName = std::string(op.symbolText());
                        if (opName.empty())
                        {
                            state.opError(source, op, "kDpicImport missing symbol");
                            state.failed = true;
                            break;
                        }
                        const wolvrix::lib::grh::OperationId existing = target.findOperation(opName);
                        if (existing.valid())
                        {
                            const wolvrix::lib::grh::Operation existingOp = target.getOperation(existing);
                            if (existingOp.kind() != wolvrix::lib::grh::OperationKind::kDpicImport)
                            {
                                state.opError(source, op,
                                              "kDpicImport symbol conflicts with existing operation");
                                state.failed = true;
                                break;
                            }
                            if (!dpiAttrsEqual(op, existingOp))
                            {
                                state.opError(source, op,
                                              "kDpicImport attributes conflict for symbol " + opName);
                                state.failed = true;
                                break;
                            }
                            if (neededOpSymbols.contains(op.symbol().value))
                            {
                                opRename.emplace(op.symbol().value, existingOp.symbol());
                            }
                            continue;
                        }
                        if (target.findValue(opName).valid())
                        {
                            state.opError(source, op,
                                          "kDpicImport symbol conflicts with existing value");
                            state.failed = true;
                            break;
                        }
                        wolvrix::lib::grh::SymbolId sym = target.internSymbol(opName);
                        if (!sym.valid())
                        {
                            state.opError(source, op,
                                          "kDpicImport symbol collision during flatten");
                            state.failed = true;
                            break;
                        }
                        wolvrix::lib::grh::OperationId newOp =
                            target.createOperation(wolvrix::lib::grh::OperationKind::kDpicImport, sym);
                        state.plan.boundaryOperation(newOp);
                        ++state.metrics.operationsCloned;
                        copyOperationSemantics(opId, newOp);
                        target.reserveOpAttrCapacity(newOp, op.attrs().size());
                        target.copyOpSrcLoc(newOp, source, opId, srcLocImport);
                        for (const auto &attr : op.attrs())
                        {
                            if (wolvrix::lib::ingest::node::isAttribute(attr.key) ||
                                attr.key == kOriginModulesKey) continue;
                            target.setAttr(newOp, attr.key, attr.value);
                        }
                        if (neededOpSymbols.contains(op.symbol().value))
                        {
                            opRename.emplace(op.symbol().value, sym);
                        }
                        state.changed = true;
                        continue;
                    }

                    const wolvrix::lib::grh::SymbolId sourceSym = source.operationSymbol(opId);
                    const std::string_view sourceName = source.symbolText(sourceSym);
                    const auto newSym = bulk ? runSymbols[runIndex] : planOperationSymbol(opId);

                    if (neededOpSymbols.contains(sourceSym.value) && !sourceName.empty())
                    {
                        const std::string_view newOpName = target.symbolText(newSym);
                        if (!newOpName.empty())
                        {
                            opRename.emplace(sourceSym.value, newSym);
                        }
                    }

                    wolvrix::lib::grh::OperationId newOp = bulk ? runOutput[runIndex] :
                        (newSym.valid() ? target.createOperation(kind, newSym) :
                                          target.createTemporaryOperation(kind));
                    ++state.metrics.operationsCloned;
                    if (!bulk) copyOperationSemantics(opId, newOp);
                    const auto sourceAttrs = source.opAttrRange(opId);
                    if (bulk && state.plan.enabled)
                    {
                        // Metadata payload records are filled as one global
                        // batch after every event has published its ID range.
                        // Resolve references at InstanceExit to retain the
                        // original diagnostic boundary and symbol precedence.
                        for (const auto attr : sourceAttrs)
                        {
                            if (!isOpSymbolReferenceAttr(attr.key())) continue;
                            if (const auto text = attr.value().get<std::string>())
                                pendingAttrs.push_back({newOp, std::string(attr.key()), std::string(*text),
                                    source.lookupSymbol(*text), true});
                        }
                        state.changed = true;
                        continue;
                    }
                    if (!bulk)
                    {
                        target.reserveOpOperandCapacity(newOp, source.opOperands(opId).size());
                        target.reserveOpResultCapacity(newOp, source.opResults(opId).size());
                    }
                    target.reserveOpAttrCapacity(newOp, sourceAttrs.size());
                    if (!bulk) target.copyOpSrcLoc(newOp, source, opId, srcLocImport);

                    for (const auto &attr : sourceAttrs)
                    {
                        if (wolvrix::lib::ingest::node::isAttribute(attr.key()) ||
                            attr.key() == kOriginModulesKey)
                        {
                            if (bulk) target.eraseAttr(newOp, attr.key());
                            continue;
                        }
                        if (!bulk || isOpSymbolReferenceAttr(attr.key()))
                            rewriteAttr(newOp, attr.key(), attr.value());
                    }

                    if (!bulk)
                    {
                        for (const auto operand : source.opOperands(opId))
                        {
                            const auto mapped = valueMap.get(operand);
                            if (!mapped.valid())
                            {
                                state.opError(source, source.getOperation(opId), "Flatten missing operand mapping");
                                state.failed = true;
                                break;
                            }
                            target.addOperand(newOp, mapped);
                        }
                        if (state.failed)
                        {
                            break;
                        }
                        for (const auto result : source.opResults(opId))
                        {
                            const auto mapped = valueMap.get(result);
                            if (!mapped.valid())
                            {
                                state.opError(source, source.getOperation(opId), "Flatten missing result mapping");
                                state.failed = true;
                                break;
                            }
                            if (!addResultChecked(state, target, newOp, mapped, source, opId, result, prefix))
                            {
                                state.failed = true;
                                break;
                            }
                        }
                        if (state.failed)
                        {
                            break;
                        }
                    }
                    state.changed = true;
                }
            }
            if (state.profile)
            {
                state.metrics.operations += FlattenClock::now() - operationsStarted;
            }
            state.metrics.pendingAttributes += pendingAttrs.size();

            if (!state.failed && !pendingAttrs.empty())
            {
                for (const auto &pending : pendingAttrs)
                {
                    auto it = opRename.find(pending.sourceSymbol.value);
                    if (it == opRename.end())
                    {
                        state.graphError(source, "Flatten unresolved symbol reference: " + pending.oldValue);
                        state.failed = true;
                        break;
                    }
                    if (!pending.deferred)
                        target.setAttr(pending.op, pending.key, std::string(target.symbolText(it->second)));
                }
            }

            if (!state.failed && !source.generateGroups().empty())
            {
                ++state.metrics.generateGroups;
                if (state.profile)
                {
                    const auto started = FlattenClock::now();
                    propagateGenerateGroups(source, target, prefix, valueRename, opRename, state);
                    state.metrics.groups += FlattenClock::now() - started;
                }
                else
                {
                    propagateGenerateGroups(source, target, prefix, valueRename, opRename, state);
                }
            }
            if (state.plan.enabled) state.plan.instances[plannedInstance].opRename = std::move(opRename);

            state.stack.erase(&source);
            if (!state.failed) state.plan.consume(GlobalInlinePlan::EventKind::Exit, plannedInstance);
            return !state.failed;
        }

        void commitGlobalMetadata(InlineState &state, wolvrix::lib::grh::Graph &target)
        {
            if (state.plan.operands.empty() || state.plan.metadataStarted) return;
            state.plan.metadataStarted = true;
            std::vector<wolvrix::lib::grh::Graph::OperationBlockImport> imports;
            imports.reserve(state.plan.operands.size());
            for (const auto &block : state.plan.operands)
            {
                const auto id = block.source->id();
                const uint64_t key = (static_cast<uint64_t>(id.index) << 32) | id.generation;
                imports.push_back({block.source, block.sourceOps, block.operations, &state.blockImports.at(key)});
            }
            target.appendOperationBlocksAttributes(imports, *state.executor);
            // Pool allocation/publication has joined. Symbol references now
            // observe the final symbol map and result positions.
            for (const auto &block : state.plan.operands)
            {
                auto &instance = state.plan.instances[block.instance];
                for (std::size_t i = 0; i < block.operations.size(); ++i)
                {
                    const auto sourceOp = block.sourceOps[i];
                    const auto targetOp = block.operations[i];
                    for (const auto attr : block.source->opAttrRange(sourceOp))
                    {
                        if (wolvrix::lib::ingest::node::isAttribute(attr.key()) ||
                            attr.key() == kOriginModulesKey)
                        {
                            target.eraseAttr(targetOp, attr.key());
                            continue;
                        }
                        if (!isOpSymbolReferenceAttr(attr.key())) continue;
                        if (const auto text = attr.value().get<std::string>())
                        {
                            const auto sourceSymbol = block.source->lookupSymbol(*text);
                            const auto found = instance.opRename.find(sourceSymbol.value);
                            if (found != instance.opRename.end())
                                target.setAttr(targetOp, attr.key(), std::string(target.symbolText(found->second)));
                        }
                    }
                }
            }
        }

        void consumeSingleUseTopology(InlineState &state, wolvrix::lib::grh::Graph &target)
        {
            if (!state.plan.enabled || state.failed) return;
            std::unordered_set<const wolvrix::lib::grh::Graph *> retained;
            for (const auto op : target.operations())
            {
                if (target.opKind(op) != wolvrix::lib::grh::OperationKind::kBlackbox) continue;
                const auto module = getAttrStringBorrowed(target.opAttrRange(op), "moduleName");
                if (const auto *source = module ? state.design.findGraph(*module) : nullptr)
                    retained.insert(source);
            }
            struct Consumption
            {
                wolvrix::lib::grh::Graph *source;
                std::vector<wolvrix::lib::grh::Graph::TopologyBlock> topology;
                std::vector<wolvrix::lib::grh::ValueId> values;
                std::vector<std::size_t> blocks;
            };
            std::vector<Consumption> jobs;
            std::unordered_map<const wolvrix::lib::grh::Graph *, std::size_t> bySource;
            // Prepare every source-derived span and mapping before consuming
            // any graph. After this loop, only the target and saved integers
            // are consulted; nodeInventory already owns its imported records.
            for (std::size_t index = 0; index < state.plan.operands.size(); ++index)
            {
                const auto &block = state.plan.operands[index];
                if (state.plan.definitionUses.at(block.source) != 1 || retained.contains(block.source)) continue;
                const auto [found, inserted] = bySource.try_emplace(block.source, jobs.size());
                if (inserted)
                {
                    const auto &instance = state.plan.instances[block.instance];
                    auto *source = state.design.findGraph(block.source->symbol());
                    if (!source || source == &target)
                        throw std::logic_error("Flatten consuming topology requires an owned child graph");
                    Consumption job{source, {}, {}, {}};
                    std::size_t count = 1;
                    for (const auto value : source->values())
                        count = std::max(count, static_cast<std::size_t>(value.index) + 1);
                    job.values.resize(count);
                    for (const auto value : source->values())
                        job.values[value.index] = state.plan.ports.get(instance.nodeBase + value.index);
                    jobs.push_back(std::move(job));
                }
                auto &job = jobs[found->second];
                job.topology.push_back({block.sourceOps, block.operations});
                job.blocks.push_back(index);
            }
            for (auto &job : jobs)
            {
                target.consumeBlockTopology(*job.source, job.topology, job.values);
                // The API deleted job.source. Do not dereference any source
                // pointer/span saved in completed plan or import contexts.
                for (const auto block : job.blocks) state.plan.operands[block].consumed = true;
            }
        }

        bool hasXmrOps(const wolvrix::lib::grh::Graph &graph)
        {
            for (const auto opId : graph.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::OperationKind kind = graph.opKind(opId);
                if (kind == wolvrix::lib::grh::OperationKind::kXMRRead ||
                    kind == wolvrix::lib::grh::OperationKind::kXMRWrite)
                {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    HierFlattenPass::HierFlattenPass()
        : Pass("hier-flatten", "hier-flatten", "Flatten module hierarchy into a single graph"),
          options_({})
    {
    }

    HierFlattenPass::HierFlattenPass(HierFlattenOptions options)
        : Pass("hier-flatten", "hier-flatten", "Flatten module hierarchy into a single graph"),
          options_(options)
    {
    }

    PassResult HierFlattenPass::run()
    {
        PassResult result;
        const char *profileEnv = std::getenv("WOLVRIX_HIER_FLATTEN_PROFILE");
        const bool profile = profileEnv && std::string_view(profileEnv) == "1";
        const auto passStarted = profile ? FlattenClock::now() : FlattenClock::time_point{};
        FlattenMetrics metrics;
        const auto emitProfile = [&]() {
            if (!profile)
            {
                return;
            }
            metrics.total = FlattenClock::now() - passStarted;
            const auto millis = [](std::chrono::nanoseconds value) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(value).count();
            };
            info("profile source-calls=" + std::to_string(metrics.sourceCalls) +
                 " instance-calls=" + std::to_string(metrics.instanceCalls) +
                 " values-visited=" + std::to_string(metrics.valuesVisited) +
                 " values-cloned=" + std::to_string(metrics.valuesCloned) +
                 " ops-visited=" + std::to_string(metrics.operationsVisited) +
                 " ops-cloned=" + std::to_string(metrics.operationsCloned) +
                 " instance-ops=" + std::to_string(metrics.instanceOperations) +
                 " annotated-ops=" + std::to_string(metrics.annotatedOperations) +
                 " qualification-lookups=" + std::to_string(metrics.qualificationLookups) +
                 " qualification-hits=" + std::to_string(metrics.qualificationHits) +
                 " qualification-misses=" + std::to_string(metrics.qualificationMisses) +
                 " qualification-input-bytes=" + std::to_string(metrics.qualificationInputBytes) +
                 " qualification-output-bytes=" + std::to_string(metrics.qualificationOutputBytes) +
                 " pending-attrs=" + std::to_string(metrics.pendingAttributes) +
                 " generate-groups=" + std::to_string(metrics.generateGroups) +
                 " semantic-nodes-copied=" + std::to_string(metrics.semanticNodesCopied) +
                 " semantic-name-bytes-copied=" + std::to_string(metrics.semanticNameBytesCopied) +
                 " semantic-copy-ms=" + std::to_string(millis(metrics.semanticCopy)) +
                 " freeze-ms=" + std::to_string(millis(metrics.freeze)) +
                 " capacity-ms=" + std::to_string(millis(metrics.capacity)) +
                 " top-stamp-ms=" + std::to_string(millis(metrics.topStamp)) +
                 " inventory-ms=" + std::to_string(millis(metrics.inventory)) +
                 " values-ms=" + std::to_string(millis(metrics.values)) +
                 " symbols-ms=" + std::to_string(millis(metrics.symbols)) +
                 " op-loop-ms=" + std::to_string(millis(metrics.operations)) +
                 " recursive-ms=" + std::to_string(millis(metrics.recursiveInstances)) +
                 " qualify-ms=" + std::to_string(millis(metrics.qualification)) +
                 " groups-ms=" + std::to_string(millis(metrics.groups)) +
                 " flush-ms=" + std::to_string(millis(metrics.flush)) +
                 " total-ms=" + std::to_string(millis(metrics.total)));
        };
        const std::size_t graphCount = design().graphs().size();
        logDebug("begin graphs=" + std::to_string(graphCount));

        for (const auto &entry : design().graphs())
        {
            if (!entry.second)
            {
                continue;
            }
            if (hasXmrOps(*entry.second))
            {
                error(*entry.second, "hier-flatten requires xmr-resolve before flatten");
                result.failed = true;
                return result;
            }
        }

        wolvrix::lib::grh::Graph *top = nullptr;
        const auto &tops = design().topGraphs();
        if (tops.size() == 1)
        {
            top = design().findGraph(tops.front());
        }
        else if (tops.empty() && design().graphs().size() == 1)
        {
            top = design().graphs().begin()->second.get();
        }
        else
        {
            error("hier-flatten requires exactly one top module");
            result.failed = true;
            return result;
        }
        if (!top)
        {
            error("hier-flatten failed to resolve top module");
            result.failed = true;
            return result;
        }

        SourceFreezeStats sourceFreeze;
        if (profile)
        {
            const auto started = FlattenClock::now();
            sourceFreeze = freezeInlineSources(design(), *top);
            metrics.freeze = FlattenClock::now() - started;
        }
        else
        {
            sourceFreeze = freezeInlineSources(design(), *top);
        }
        info("source freeze eligible=" + std::to_string(sourceFreeze.eligible) +
             " frozen=" + std::to_string(sourceFreeze.frozen) +
             " already-frozen=" + std::to_string(sourceFreeze.alreadyFrozen) +
             " skipped-positional=" + std::to_string(sourceFreeze.skippedPositional) +
             " failed-freeze=" + std::to_string(sourceFreeze.failedFreeze));

        wolvrix::lib::ingest::node::InventoryImportBatch nodeInventory(*top);
        InlineState state{design(), nodeInventory, metrics};
        state.profile = profile;
        state.numThreads = options_.numThreads ? options_.numThreads :
            std::max<std::size_t>(1, std::min<std::size_t>(10, std::thread::hardware_concurrency()));
        state.symProtect = options_.symProtect;
        state.graphError = [this](const wolvrix::lib::grh::Graph &graph, std::string message) {
            error(graph, std::move(message));
        };
        state.opError = [this](const wolvrix::lib::grh::Graph &graph,
                               const wolvrix::lib::grh::Operation &op,
                               std::string message) {
            error(graph, op, std::move(message));
        };

        // Invariant: top-graph symbols are never renamed; they are the root of
        // every hierarchical path. Only symbols cloned from inlined child
        // graphs receive the `inst$...$` prefix (and only child graphs'
        // generate groups gain the scope prefix).
        const std::string topName = top->symbol();
        if (top->semanticTree()) state.currentSemanticParent = top->semanticTree()->root();

        DenseValueMap identity(*top);
        for (const auto valueId : top->values())
        {
            if (!valueId.valid())
            {
                continue;
            }
            identity.set(valueId, valueId);
        }

        // Prefixing happens only while inlining a kInstance op, and the op is
        // erased as it is inlined; re-running the pass on an already flattened
        // graph finds no instances and is a no-op, so already-prefixed symbols
        // never gain a second prefix.
        std::vector<wolvrix::lib::grh::OperationId> instanceOps;
        for (const auto opId : top->operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            if (top->opKind(opId) == wolvrix::lib::grh::OperationKind::kInstance)
            {
                instanceOps.push_back(opId);
            }
        }

        if (options_.useGlobalPlan) state.plan = planGlobalInline(design(), *top, instanceOps);
        if (state.plan.enabled)
        {
            state.executor = std::make_unique<wolvrix::lib::grh::detail::BlockExecutor>(state.numThreads);
            identity.globalPorts = &state.plan.ports;
            for (const auto value : top->values()) identity.set(value, value);
        }

        // Flattening appends every reachable child value/operation to the top
        // builder. Reserve the recursively expanded shape once so the large
        // vectors do not repeatedly reallocate while each instance is cloned.
        // The estimate is read-only and uses the same moduleName edges as the
        // inline path; a recursive edge is left to the existing diagnostic.
        std::unordered_map<const wolvrix::lib::grh::Graph *, FlattenCapacityEstimate> capacityMemo;
        std::unordered_set<const wolvrix::lib::grh::Graph *> capacityStack;
        FlattenCapacityEstimate capacity;
        if (profile)
        {
            const auto started = FlattenClock::now();
            capacity = estimateFlattenCapacity(design(), *top, capacityMemo, capacityStack);
            metrics.capacity = FlattenClock::now() - started;
        }
        else
        {
            capacity = estimateFlattenCapacity(design(), *top, capacityMemo, capacityStack);
        }
        top->reserveValueCapacity(capacity.values);
        top->reserveOperationCapacity(capacity.operations);
        top->reserveSymbolCapacity(capacity.symbols);
        logDebug("reserved flatten values=" + std::to_string(capacity.values) +
                 " operations=" + std::to_string(capacity.operations) +
                 " symbols=" + std::to_string(capacity.symbols));

        try
        {
            for (std::size_t instance = 0; instance < instanceOps.size(); ++instance)
            {
                const auto opId = instanceOps[instance];
                if (state.failed)
                {
                    break;
                }
                state.plan.consume(GlobalInlinePlan::EventKind::Boundary, GlobalPortUnionFind::invalid, instance);
                const InstanceInfo info = buildInstanceInfo(*top, opId);
                top->eraseOpUnchecked(opId);
                state.changed = true;
                ++metrics.instanceCalls;
                ++metrics.instanceOperations;
                if (profile)
                {
                    const auto started = FlattenClock::now();
                    if (!inlineInstanceInfo(*top, info, identity, *top, std::string(), state))
                    {
                        state.failed = true;
                        break;
                    }
                    metrics.recursiveInstances += FlattenClock::now() - started;
                }
                else if (!inlineInstanceInfo(*top, info, identity, *top, std::string(), state))
                {
                    state.failed = true;
                    break;
                }
            }
            if (state.plan.enabled && !state.failed &&
                (state.plan.nextEvent != state.plan.events.size() ||
                 state.plan.nextInstance != state.plan.instances.size()))
                throw std::logic_error("Flatten global event plan was not fully consumed");
            commitGlobalMetadata(state, *top);
            if (!options_.preserveFlattenedModules) consumeSingleUseTopology(state, *top);
            state.plan.commitOperands(*top, state.executor.get());
        }
        catch (...)
        {
            commitGlobalMetadata(state, *top);
            state.plan.commitOperands(*top, state.executor.get());
            top->rebuildUseLists();
            throw;
        }
        top->rebuildUseLists();

        if (state.failed)
        {
            if (profile)
            {
                const auto started = FlattenClock::now();
                nodeInventory.flush();
                metrics.flush += FlattenClock::now() - started;
            }
            else
            {
                nodeInventory.flush();
            }
            result.failed = true;
            emitProfile();
            return result;
        }

        if (!options_.preserveFlattenedModules)
        {
            std::unordered_set<std::string> keepModules;
            for (const auto opId : top->operations())
            {
                if (top->opKind(opId) != wolvrix::lib::grh::OperationKind::kBlackbox)
                {
                    continue;
                }
                const wolvrix::lib::grh::Operation op = top->getOperation(opId);
                const auto attr = op.attr("moduleName");
                if (!attr)
                {
                    continue;
                }
                if (const auto value = std::get_if<std::string>(&*attr))
                {
                    if (!value->empty())
                    {
                        keepModules.insert(*value);
                    }
                }
            }
            std::vector<std::string> toDelete;
            toDelete.reserve(design().graphs().size());
            for (const auto &entry : design().graphs())
            {
                if (!entry.second)
                {
                    continue;
                }
                if (entry.first != topName && keepModules.find(entry.first) == keepModules.end())
                {
                    toDelete.push_back(entry.first);
                }
            }
            for (const auto &name : toDelete)
            {
                design().deleteGraph(name);
            }
        }

        if (profile)
        {
            const auto started = FlattenClock::now();
            nodeInventory.flush();
            metrics.flush += FlattenClock::now() - started;
        }
        else
        {
            nodeInventory.flush();
        }
        result.changed = state.changed;
        emitProfile();
        return result;
    }

} // namespace wolvrix::lib::transform
