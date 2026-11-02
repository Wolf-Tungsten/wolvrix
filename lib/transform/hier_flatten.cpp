#include "transform/hier_flatten.hpp"

#include "core/grh.hpp"
#include "core/transform.hpp"
#include "ingest/node_attributes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
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
            std::size_t pendingAttributes = 0;
            std::size_t generateGroups = 0;
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
                return source.valid() && source.graph == graph && source.generation == 0 &&
                       source.index < values.size() && values[source.index].valid();
            }

            wolvrix::lib::grh::ValueId get(wolvrix::lib::grh::ValueId source) const noexcept
            {
                if (!source.valid() || source.graph != graph || source.generation != 0 ||
                    source.index >= values.size())
                {
                    return wolvrix::lib::grh::ValueId::invalid();
                }
                return values[source.index];
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
            }
        };

        struct PendingAttr
        {
            wolvrix::lib::grh::OperationId op;
            std::string key;
            std::string oldValue;
        };

        bool isOpSymbolReferenceAttr(std::string_view key)
        {
            return key == "regSymbol" || key == "latchSymbol" || key == "targetImportSymbol";
        }

        struct InstanceInfo
        {
            std::string moduleName;
            std::string instanceName;
            std::vector<wolvrix::lib::grh::ValueId> operands;
            std::vector<wolvrix::lib::grh::ValueId> results;
            std::optional<std::vector<std::string>> inputPortNames;
            std::optional<std::vector<std::string>> outputPortNames;
            std::optional<std::vector<std::string>> inoutPortNames;
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
            std::function<void(const wolvrix::lib::grh::Graph &, std::string)> graphError;
            std::function<void(const wolvrix::lib::grh::Graph &, const wolvrix::lib::grh::Operation &,
                               std::string)> opError;
            HierFlattenOptions::SymProtectMode symProtect = HierFlattenOptions::SymProtectMode::All;
            // Instance path of the graph currently being inlined; synthetic
            // assigns created on its behalf are stamped with it.
            std::string currentPrefix;
            std::vector<std::string> nodeInstancePath;
            bool profile = false;
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
            std::span<const wolvrix::lib::grh::AttrKV> attrs, std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key == key)
                {
                    if (const auto *value = std::get_if<std::string>(&entry.value))
                    {
                        return *value;
                    }
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        const std::vector<std::string> *getAttrStringsBorrowed(
            std::span<const wolvrix::lib::grh::AttrKV> attrs, std::string_view key)
        {
            for (const auto &entry : attrs)
            {
                if (entry.key == key)
                {
                    return std::get_if<std::vector<std::string>>(&entry.value);
                }
            }
            return nullptr;
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
                for (const auto &attr : source.opAttrs(opId))
                {
                    if (!isOpSymbolReferenceAttr(attr.key))
                    {
                        continue;
                    }
                    if (const auto *name = std::get_if<std::string>(&attr.value))
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

        bool hasNamedPortMaps(std::span<const wolvrix::lib::grh::AttrKV> attrs,
                              const wolvrix::lib::grh::Graph &source)
        {
            const auto named = [&](std::string_view key, const auto &ports) {
                const auto *names = getAttrStringsBorrowed(attrs, key);
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
                    const auto attrs = graph->opAttrs(opId);
                    const auto moduleName = getAttrStringBorrowed(attrs, "moduleName");
                    auto *source = moduleName ? design.findGraph(*moduleName) : nullptr;
                    if (!source || source == &top) continue;
                    auto &references = sources[source];
                    references.inlineCandidate |= kind == wolvrix::lib::grh::OperationKind::kInstance;
                    references.named = references.named && hasNamedPortMaps(attrs, *source);
                }
            }
            // Freeze each read-only definition before retaining any flatten
            // IDs or spans. This releases dead builder storage without keeping
            // the builders alive alongside the growing flattened top. Freeze
            // sorts ports, so every reference must use complete named maps.
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
                    ++stats.alreadyFrozen;
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
                const auto attrs = graph.opAttrs(opId);
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
            std::string out(prefix);
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
            std::string candidate = base;
            int suffix = 0;
            while (true)
            {
                wolvrix::lib::grh::SymbolId sym = graph.internSymbol(candidate);
                if (sym.valid())
                {
                    return sym;
                }
                candidate = base + "_" + std::to_string(++suffix);
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
                message.append(dstValue.symbolText());
                message.append(" target_graph=");
                message.append(target.symbol());
                if (srcValue.valid())
                {
                    const wolvrix::lib::grh::Value srcVal = source.getValue(srcValue);
                    message.append(" source_value=");
                    message.append(srcVal.symbolText());
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
                message.append(existing.symbolText());
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
                target.createValue(target.makeInternalValSym(), expected);
            const wolvrix::lib::grh::OperationId assign =
                target.createOperation(wolvrix::lib::grh::OperationKind::kAssign,
                                       target.makeInternalOpSym());
            target.reserveOpOperandCapacity(assign, 1);
            target.reserveOpResultCapacity(assign, 1);
            target.reserveOpAttrCapacity(assign, 1);
            target.addOperand(assign, targetValue);
            target.addResult(assign, view);
            // Synthetic shape-view assign exists because of this instance.
            target.setAttr(assign, kOriginModulesKey,
                           std::vector<std::string>{state.currentPrefix});
            views.emplace_back(expected, view);
            state.changed = true;
            return view;
        }

        bool mapOutputPortType(wolvrix::lib::grh::Graph &target,
                               const wolvrix::lib::grh::Graph &child,
                               wolvrix::lib::grh::ValueId childValue,
                               wolvrix::lib::grh::ValueId targetValue,
                               DenseValueMap &portMap, InlineState &state, const std::string &prefix)
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
                found = sourceType == targetType ? targetValue : target.createValue(sourceType);
                portMap.set(childValue, found);
            }
            if (found != targetValue)
            {
                if (target.valueDef(targetValue))
                {
                    state.graphError(child, "Flatten output alias already has a defining op");
                    state.failed = true;
                    return false;
                }
                const auto assign = target.createOperation(wolvrix::lib::grh::OperationKind::kAssign);
                target.reserveOpOperandCapacity(assign, 1);
                target.reserveOpResultCapacity(assign, 1);
                target.reserveOpAttrCapacity(assign, 1);
                target.addOperand(assign, found);
                target.addResult(assign, targetValue);
                target.setAttr(assign, kOriginModulesKey, std::vector<std::string>{prefix});
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
            state.currentPrefix = childPrefix;

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
                    childPortMap.set(childValue, mappedInput);
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
                if (!mapOutputPortType(target, *child, childValue, *targetValue, childPortMap, state, childPrefix))
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
                if (!mapOutputPortType(target, *child, childInout.out, *outValue, childPortMap, state, childPrefix) ||
                    !mapOutputPortType(target, *child, childInout.oe, *oeValue, childPortMap, state, childPrefix))
                    return false;
                if (!childPortMap.contains(childInout.in))
                {
                    childPortMap.set(childInout.in, mappedInput);
                }
            }
            if (state.failed)
            {
                return false;
            }

            state.nodeInstancePath.push_back(component);
            const bool success = inlineGraphContents(*child, target, childPrefix, std::move(childPortMap), state);
            state.nodeInstancePath.pop_back();
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
            const auto attrs = graph.opAttrs(opId);
            info.moduleName = getAttrStringBorrowed(attrs, "moduleName").value_or(std::string_view());
            info.instanceName = getAttrStringBorrowed(attrs, "instanceName").value_or(std::string_view());
            const auto operands = graph.opOperands(opId);
            const auto results = graph.opResults(opId);
            info.operands.assign(operands.begin(), operands.end());
            info.results.assign(results.begin(), results.end());
            if (const auto *names = getAttrStringsBorrowed(attrs, "inputPortName"))
            {
                info.inputPortNames = *names;
            }
            if (const auto *names = getAttrStringsBorrowed(attrs, "outputPortName"))
            {
                info.outputPortNames = *names;
            }
            if (const auto *names = getAttrStringsBorrowed(attrs, "inoutPortName"))
            {
                info.inoutPortNames = *names;
            }
            return info;
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
                                     const std::unordered_map<std::string, std::string> &valueRename,
                                     const std::unordered_map<std::string, std::string> &opRename,
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
                    const std::string oldText = std::string(source.symbolText(srcSym));
                    if (oldText.empty())
                    {
                        continue;
                    }
                    std::string_view newText;
                    if (auto it = valueRename.find(oldText); it != valueRename.end())
                    {
                        newText = it->second;
                    }
                    else if (auto it = opRename.find(oldText); it != opRename.end())
                    {
                        newText = it->second;
                    }
                    else
                    {
                        // Symbol was dropped while inlining (port mapping, etc.).
                        continue;
                    }
                    const wolvrix::lib::grh::SymbolId dstSym = target.lookupSymbol(newText);
                    if (!dstSym.valid())
                    {
                        state.graphError(source,
                                         "Flatten generate group member missing after rename: " +
                                             std::string(newText));
                        state.failed = true;
                        return false;
                    }
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
            ++state.metrics.sourceCalls;
            if (state.profile)
            {
                const auto started = FlattenClock::now();
                state.nodeInventory.record(source, state.nodeInstancePath);
                state.metrics.inventory += FlattenClock::now() - started;
            }
            else
            {
                state.nodeInventory.record(source, state.nodeInstancePath);
            }
            wolvrix::lib::ingest::node::QualificationContext nodeQualification(state.nodeInstancePath);

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
            std::unordered_map<std::string, std::string> valueRename;
            const auto valuesStarted = state.profile ? FlattenClock::now() : FlattenClock::time_point{};
            state.metrics.valuesVisited += source.values().size();
            for (const auto valueId : source.values())
            {
                if (!valueId.valid())
                {
                    continue;
                }
                if (valueMap.contains(valueId))
                {
                    continue;
                }
                const int32_t width = source.valueWidth(valueId);
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
                if (!newSym.valid())
                {
                    newSym = target.makeInternalValSym();
                }
                // stage-04: preserve the array view on cloned values.
                wolvrix::lib::grh::ValueId newValue =
                    target.createValue(newSym, width, source.valueSigned(valueId),
                                       source.valueType(valueId), source.valueShape(valueId),
                                       source.valueDomain(valueId));
                if (auto srcLoc = source.valueSrcLoc(valueId))
                {
                    target.setValueSrcLoc(newValue, std::move(*srcLoc));
                }
                valueMap.set(valueId, newValue);
                ++state.metrics.valuesCloned;
                if (trackValueRename && !sourceName.empty())
                {
                    valueRename.emplace(std::string(sourceName),
                                        std::string(target.symbolText(newSym)));
                }
                state.changed = true;
            }
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

            std::unordered_map<std::string, std::string> opRename;
            opRename.reserve(neededOpSymbols.size());
            std::vector<PendingAttr> pendingAttrs;

            auto rewriteAttr = [&](wolvrix::lib::grh::OperationId newOp,
                                   const std::string &key,
                                   const wolvrix::lib::grh::AttributeValue &value) {
                if (const auto *strValue = std::get_if<std::string>(&value);
                    strValue && isOpSymbolReferenceAttr(key))
                {
                    auto it = opRename.find(*strValue);
                    if (it != opRename.end())
                    {
                        target.setAttr(newOp, key, it->second);
                        return;
                    }
                    pendingAttrs.push_back(PendingAttr{newOp, key, *strValue});
                }
                target.setAttr(newOp, key, value);
            };

            const auto operationsStarted = state.profile ? FlattenClock::now() : FlattenClock::time_point{};
            state.metrics.operationsVisited += source.operations().size();
            for (const auto opId : source.operations())
            {
                if (!opId.valid())
                {
                    continue;
                }
                const wolvrix::lib::grh::OperationKind kind = source.opKind(opId);
                if (kind == wolvrix::lib::grh::OperationKind::kInstance)
                {
                    ++state.metrics.instanceCalls;
                    ++state.metrics.instanceOperations;
                    const InstanceInfo info = buildInstanceInfo(source, opId);
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
                        // Same import declared in another module: union the
                        // provenance instead of dropping it. A child op
                        // without originModules belongs to the graph being
                        // inlined (same convention as the top-graph stamping).
                        std::vector<std::string> origins =
                            getOriginModules(source, op.id()).value_or(std::vector<std::string>{prefix});
                        if (!origins.empty())
                        {
                            std::vector<std::string> merged =
                                getOriginModules(target, existing).value_or(std::vector<std::string>{});
                            for (const std::string &module : origins)
                            {
                                appendOriginModule(merged, module);
                            }
                            target.setAttr(existing, kOriginModulesKey, std::move(merged));
                        }
                        if (neededOpSymbols.contains(op.symbol().value))
                        {
                            opRename.emplace(opName, opName);
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
                    ++state.metrics.operationsCloned;
                    target.reserveOpAttrCapacity(newOp, op.attrs().size());
                    if (op.srcLoc())
                    {
                        target.setOpSrcLoc(newOp, *op.srcLoc());
                    }
                    for (const auto &attr : op.attrs())
                    {
                        target.setAttr(newOp, attr.key, attr.value);
                    }
                    if (neededOpSymbols.contains(op.symbol().value))
                    {
                        opRename.emplace(opName, opName);
                    }
                    state.changed = true;
                    continue;
                }

                const wolvrix::lib::grh::SymbolId sourceSym = source.operationSymbol(opId);
                const std::string_view sourceName = source.symbolText(sourceSym);
                wolvrix::lib::grh::SymbolId newSym = wolvrix::lib::grh::SymbolId::invalid();
                const bool protectOp = shouldProtectOp(sourceSym);
                if (protectOp)
                {
                    if (!sourceName.empty())
                    {
                        const std::string hierName = makeHierName(prefix, sourceName);
                        newSym = internUniqueSymbol(target, hierName);
                        target.addDeclaredSymbol(newSym);
                    }
                }
                if (!newSym.valid())
                {
                    newSym = target.makeInternalOpSym();
                }

                if (neededOpSymbols.contains(sourceSym.value) && !sourceName.empty())
                {
                    const std::string_view newOpName = target.symbolText(newSym);
                    if (!newOpName.empty())
                    {
                        opRename.emplace(std::string(sourceName), std::string(newOpName));
                    }
                }

                wolvrix::lib::grh::OperationId newOp =
                    target.createOperation(kind, newSym);
                ++state.metrics.operationsCloned;
                const auto sourceAttrs = source.opAttrs(opId);
                target.reserveOpOperandCapacity(newOp, source.opOperands(opId).size());
                target.reserveOpResultCapacity(newOp, source.opResults(opId).size());
                // originModules is either replaced or appended below, so one
                // extra slot covers the common case without growing the
                // per-operation attribute vector while cloning.
                target.reserveOpAttrCapacity(newOp, sourceAttrs.size() + 1);
                if (auto srcLoc = source.opSrcLoc(opId))
                {
                    target.setOpSrcLoc(newOp, std::move(*srcLoc));
                }

                bool hasOrigins = false;
                bool hasNodeAttrs = false;
                for (const auto &attr : sourceAttrs)
                {
                    if (wolvrix::lib::ingest::node::isAttribute(attr.key))
                    {
                        hasNodeAttrs = true;
                        // The supported associations are built once below,
                        // after results have their final positions. Preserve
                        // unknown attributes for the existing verifier error.
                        if (attr.key == wolvrix::lib::ingest::node::kMembers ||
                            attr.key == wolvrix::lib::ingest::node::kOrigins ||
                            attr.key == wolvrix::lib::ingest::node::kRoots ||
                            attr.key == wolvrix::lib::ingest::node::kRootResults)
                        {
                            continue;
                        }
                    }
                    if (attr.key != kOriginModulesKey)
                    {
                        rewriteAttr(newOp, attr.key, attr.value);
                        continue;
                    }
                    // Build the final provenance once, in its original attr
                    // slot. Copying it into the target and reading it back
                    // would allocate the same strings twice per cloned op.
                    hasOrigins = true;
                    std::vector<std::string> origins;
                    if (const auto *sourceOrigins = std::get_if<std::vector<std::string>>(&attr.value))
                    {
                        origins.reserve(sourceOrigins->size() + 1);
                        origins.assign(sourceOrigins->begin(), sourceOrigins->end());
                    }
                    if (origins.empty())
                    {
                        origins.push_back(prefix);
                    }
                    else
                    {
                        prependOriginModule(origins, prefix);
                    }
                    target.setAttr(newOp, kOriginModulesKey, std::move(origins));
                }
                if (!hasOrigins)
                {
                    target.setAttr(newOp, kOriginModulesKey, std::vector<std::string>{prefix});
                }

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
                if (hasNodeAttrs)
                {
                    ++state.metrics.annotatedOperations;
                    if (state.profile)
                    {
                        const auto started = FlattenClock::now();
                        wolvrix::lib::ingest::node::copyQualified(source, opId, target, newOp, nodeQualification);
                        state.metrics.qualification += FlattenClock::now() - started;
                    }
                    else
                    {
                        wolvrix::lib::ingest::node::copyQualified(source, opId, target, newOp, nodeQualification);
                    }
                }
                state.changed = true;
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
                    auto it = opRename.find(pending.oldValue);
                    if (it == opRename.end())
                    {
                        state.graphError(source, "Flatten unresolved symbol reference: " + pending.oldValue);
                        state.failed = true;
                        break;
                    }
                    target.setAttr(pending.op, pending.key, it->second);
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

            state.stack.erase(&source);
            return !state.failed;
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
                 " pending-attrs=" + std::to_string(metrics.pendingAttributes) +
                 " generate-groups=" + std::to_string(metrics.generateGroups) +
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
        // Top-graph ops are never recreated during flatten, so stamp them
        // here: pre-existing ops without originModules belong to the top
        // module itself.
        const std::string topName = top->symbol();
        const auto topStampStarted = profile ? FlattenClock::now() : FlattenClock::time_point{};
        for (const auto opId : top->operations())
        {
            if (!opId.valid())
            {
                continue;
            }
            bool stamped = false;
            for (const auto &kv : top->opAttrs(opId))
            {
                if (kv.key == kOriginModulesKey)
                {
                    stamped = true;
                    break;
                }
            }
            if (stamped)
            {
                continue;
            }
            top->setAttr(opId, kOriginModulesKey, std::vector<std::string>{topName});
        }
        if (profile)
        {
            metrics.topStamp = FlattenClock::now() - topStampStarted;
        }

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

        for (const auto opId : instanceOps)
        {
            if (state.failed)
            {
                break;
            }
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
