#include "transform/grhsim/partition-sink.hpp"

#include "partition-internal.hpp"

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
using namespace detail;

std::string clockSignature(const Graph& graph, ValueId fire) {
    if (!fire) return "level";
    std::set<std::string> terms;
    std::unordered_set<ValueId, ValueIdHash> seen;
    std::vector<ValueId> queue{fire};
    while (!queue.empty()) {
        const auto value = queue.back(); queue.pop_back();
        if (!seen.insert(value).second) continue;
        const auto op = graph.valueDef(value);
        if (!op) throw std::invalid_argument("Unresolved clock signature");
        const auto kind = graph.opKind(op);
        if (kind == kEdgeDetect) terms.insert(expressionKey(graph, op, true));
        else if (kind == OperationKind::kAssign || kind == OperationKind::kOr)
            queue.insert(queue.end(), graph.opOperands(op).begin(), graph.opOperands(op).end());
        else if (kind != OperationKind::kConstant || !isFire(graph, value))
            throw std::invalid_argument("Invalid clock signature");
    }
    std::string signature;
    for (const auto& term : terms) signature += std::to_string(term.size()) + ":" + term;
    return signature.empty() ? "never" : signature;
}

bool isNonsink(const Owner& path) {
    return path.size() >= 4 && path[0] == "general" && path[1] == "nonsink";
}

std::string slotPrefix(const Graph& graph) {
    return "grhsim.sinkSchedule/" + std::to_string(graph.owner().instanceId()) + "/" +
        std::to_string(graph.id().index) + "/" + std::to_string(graph.id().generation) + "/";
}

uint64_t configHash(const Graph& graph) {
    const auto configuration = graph.metadata("grhsim.sinkConfig");
    if (!configuration || !std::holds_alternative<std::string>(*configuration))
        throw std::invalid_argument("Missing sink partition configuration");
    return configurationHash(std::get<std::string>(*configuration));
}

struct NonsinkDag {
    std::vector<const PartitionKey*> order;
    std::vector<std::size_t> prerequisiteOffsets, prerequisites;
    std::vector<std::size_t> operationNode;
    std::vector<uint8_t> general;
};

NonsinkDag nonsinkDag(const Graph& graph) {
    NonsinkDag result;
    uint32_t last = 0;
    for (const auto op : graph.operations()) last = std::max(last, op.index);
    result.operationNode.assign(static_cast<std::size_t>(last) + 1, SIZE_MAX);
    result.general.resize(static_cast<std::size_t>(last) + 1);
    std::vector<OperationId> operations;
    for (const auto op : graph.operations()) {
        const auto& path = ownerView(graph, op);
        result.general[op.index] = path.size() >= 2 && path[0] == "general";
        if (isNonsink(path)) operations.push_back(op);
    }
    // Borrow keys from the immutable graph and use their lexical ranks as
    // tie breakers. This preserves the published order without millions of
    // duplicate key strings and individually allocated map/set nodes.
    std::sort(operations.begin(), operations.end(), [&](auto left, auto right) {
        return ownerView(graph, left) < ownerView(graph, right);
    });
    std::vector<const PartitionKey*> keys;
    for (const auto op : operations) {
        const auto& key = ownerView(graph, op);
        if (keys.empty() || *keys.back() != key) keys.push_back(&key);
        result.operationNode[op.index] = keys.size() - 1;
    }
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (const auto op : operations) {
        const auto target = result.operationNode[op.index];
        for (const auto value : graph.opOperands(op)) {
            const auto producer = graph.valueDef(value);
            if (!producer) continue;
            const auto source = result.operationNode[producer.index];
            if (source != SIZE_MAX && source != target) edges.emplace_back(source, target);
        }
    }
    std::map<int64_t, OperationId> effects;
    for (const auto op : operations)
        if (const auto order = attribute<int64_t>(graph, op, "grhsim.effectOrder")) effects.emplace(*order, op);
    OperationId previous;
    for (const auto& [order, op] : effects) {
        if (previous) {
            const auto source = result.operationNode[previous.index];
            const auto target = result.operationNode[op.index];
            if (source != target) edges.emplace_back(source, target);
        }
        previous = op;
    }
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    std::vector<std::size_t> offsets(keys.size() + 1), degree(keys.size());
    for (const auto& [source, target] : edges) { ++offsets[source + 1]; ++degree[target]; }
    for (std::size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i - 1];
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
    for (std::size_t i = 0; i < degree.size(); ++i) if (!degree[i]) ready.push(i);
    std::vector<std::size_t> ordinal(keys.size());
    while (!ready.empty()) {
        const auto key = ready.top(); ready.pop();
        ordinal[key] = result.order.size(); result.order.push_back(keys[key]);
        for (auto i = offsets[key]; i < offsets[key + 1]; ++i)
            if (--degree[edges[i].second] == 0) ready.push(edges[i].second);
    }
    if (result.order.size() != keys.size()) throw std::invalid_argument("Cyclic nonsink partition dependencies");
    result.prerequisiteOffsets.resize(keys.size() + 1);
    for (const auto& [source, target] : edges) ++result.prerequisiteOffsets[ordinal[target] + 1];
    for (std::size_t i = 1; i < result.prerequisiteOffsets.size(); ++i)
        result.prerequisiteOffsets[i] += result.prerequisiteOffsets[i - 1];
    auto cursor = result.prerequisiteOffsets;
    result.prerequisites.resize(edges.size());
    for (const auto& [source, target] : edges) result.prerequisites[cursor[ordinal[target]]++] = ordinal[source];
    for (const auto op : operations) result.operationNode[op.index] = ordinal[result.operationNode[op.index]];
    return result;
}

std::vector<std::size_t> directDependencies(const Graph& graph, OperationId writer, const NonsinkDag& dag) {
    std::vector<std::size_t> result;
    std::vector<OperationId> queue{writer};
    std::unordered_set<OperationId, OperationIdHash> seen;
    while (!queue.empty()) {
        const auto op = queue.back(); queue.pop_back();
        if (!seen.insert(op).second) continue;
        const auto node = dag.operationNode[op.index];
        if (node != SIZE_MAX) { result.push_back(node); continue; }
        for (const auto value : graph.opOperands(op)) {
            const auto producer = graph.valueDef(value);
            if (producer) {
                if (dag.general[producer.index]) queue.push_back(producer);
            }
        }
    }
    return result;
}

SinkSchedule buildSchedule(const Graph& graph) {
    SinkSchedule schedule;
    schedule.stamp = DesignStamp::capture(graph);
    schedule.partitionConfigHash = configHash(graph);
    const auto writers = orderedWriters(graph);
    const auto nonsinks = nonsinkDag(graph);
    std::vector<std::vector<std::size_t>> runRoots;
    std::map<PartitionKey, SinkGroup> groups;
    std::map<PartitionKey, std::string> clocks;
    std::map<PartitionKey, uint32_t> groupPriorities;
    std::map<PartitionKey, bool> groupHoisting;
    std::unordered_map<OperationId, uint32_t, OperationIdHash> priorities;
    std::unordered_map<ValueId, std::string, ValueIdHash> clockCache;
    for (const auto writer : writers) {
        const auto& key = ownerView(graph, writer);
        if (key.size() != 3 || key[0] != "general" || (key[1] != "small-sink" && key[1] != "large-sink"))
            throw std::invalid_argument("Writer has no complete sink group");
        const auto args = graph.opOperands(writer);
        const auto fire = graph.opKind(writer) == kLatchWrite ? ValueId{} : args[0];
        const auto [clockEntry, freshClock] = clockCache.try_emplace(fire);
        if (freshClock) clockEntry->second = clockSignature(graph, fire);
        const auto& clock = clockEntry->second;
        const auto priority = priorities[writerTarget(graph, writer)]++;
        const bool hoist = attribute<bool>(graph, writer, "grhsim.hoistEnable").value_or(false);
        const auto [found, inserted] = groups.emplace(key, SinkGroup{key, fire, {}});
        if (inserted) {
            clocks[key] = clock; groupPriorities[key] = priority; groupHoisting[key] = hoist;
        } else {
            if (clocks.at(key) != clock) throw std::invalid_argument("Sink group mixes clocks");
            if (groupPriorities.at(key) != priority) throw std::invalid_argument("Sink group mixes writer priority ranks");
            if (groupHoisting.at(key) != hoist) throw std::invalid_argument("Sink group mixes hoisted and local guards");
        }
        if (hoist) {
            const auto guard = args[graph.opKind(writer) == kLatchWrite ? 0 : 1];
            if (!found->second.guards.empty() && found->second.guards.front() != guard)
                throw std::invalid_argument("Sink group mixes hoisted guards");
            found->second.guards = {guard};
        }
        if (schedule.generalRuns.empty() || schedule.generalRuns.back().partitionKey != key) {
            schedule.generalRuns.push_back({key, {}, {}});
            runRoots.emplace_back();
        }
        auto& run = schedule.generalRuns.back();
        run.orderedWriters.push_back(writer);
        const auto required = directDependencies(graph, writer, nonsinks);
        runRoots.back().insert(runRoots.back().end(), required.begin(), required.end());
        if (key[1] == "large-sink") {
            if (schedule.largeCommitRuns.empty() || schedule.largeCommitRuns.back().partitionKey != key)
                schedule.largeCommitRuns.push_back({key, {}, {}});
            schedule.largeCommitRuns.back().orderedWriters.push_back(writer);
        }
    }
    // A run can contain thousands of writers sharing one cone. Expand its
    // closure once using dense indices; keep text keys only in the published
    // schedule, with the same complete leaf dependency/order contract.
    std::vector<std::size_t> seen(nonsinks.order.size(), 0);
    for (std::size_t i = 0; i < schedule.generalRuns.size(); ++i) {
        auto pending = std::move(runRoots[i]);
        std::vector<std::size_t> required;
        while (!pending.empty()) {
            const auto key = pending.back(); pending.pop_back();
            if (seen[key] == i + 1) continue;
            seen[key] = i + 1; required.push_back(key);
            pending.insert(pending.end(), nonsinks.prerequisites.begin() + nonsinks.prerequisiteOffsets[key],
                           nonsinks.prerequisites.begin() + nonsinks.prerequisiteOffsets[key + 1]);
        }
        std::sort(required.begin(), required.end());
        auto& keys = schedule.generalRuns[i].requiredNonsinkKeys;
        keys.reserve(required.size());
        for (const auto key : required) keys.push_back(*nonsinks.order[key]);
    }
    for (const auto op : graph.operations()) {
        const auto& path = ownerView(graph, op);
        if (path.size() == 3 && path[0] == "general" && path[1] != "nonsink" && !groups.contains(path))
            throw std::invalid_argument("Orphaned sink group");
    }
    for (auto& [key, group] : groups) schedule.sinkGroups.push_back(std::move(group));
    schedule.complete = true;
    return schedule;
}
}

std::string sinkScheduleKey(const grh::Graph& graph) {
    return slotPrefix(graph) + std::to_string(configHash(graph));
}

std::string verifySinkSchedule(const grh::Graph& graph, const SinkSchedule& schedule) {
    try {
        if (!schedule.complete || !schedule.stamp.matches(graph)) return "Missing or stale sink schedule";
        if (stage(graph) != "sink-partitioned") return "Sink schedule requires sink-partitioned stage";
        if (const auto problem = verifyGraph(graph); !problem.empty()) return problem;
        const auto expected = buildSchedule(graph);
        if (schedule.partitionConfigHash != expected.partitionConfigHash || schedule.sinkGroups != expected.sinkGroups ||
            schedule.generalRuns != expected.generalRuns || schedule.largeCommitRuns != expected.largeCommitRuns)
            return "Sink schedule disagrees with graph ownership, guards, dependencies or writer order";
        return {};
    } catch (const std::exception& error) { return error.what(); }
}

const SinkSchedule& requireSinkSchedule(const SessionStore* session, const grh::Graph& graph) {
    const auto& schedule = requireSessionValue<SinkSchedule>(session, sinkScheduleKey(graph));
    if (const auto problem = verifySinkSchedule(graph, schedule); !problem.empty()) throw std::invalid_argument(problem);
    return schedule;
}

PartitionSinkPass::PartitionSinkPass(PartitionSinkOptions options)
    : Pass("grhsim-partition-sink", "grhsim-partition-sink", "Partition sink clocks and publish ordered writer runs"),
      options_(std::move(options)) {}

bool PartitionSinkPass::supportsDialect(std::string_view dialect) const noexcept { return supports(dialect); }

PassResult PartitionSinkPass::run() {
    PassResult result;
    Graph* graph = nullptr;
    try {
        std::string top = options_.top;
        if (top.empty() && design().topGraphs().size() == 1) top = design().topGraphs()[0];
        graph = design().findGraph(top);
        if (graph && session()) {
            const auto prefix = slotPrefix(*graph);
            std::erase_if(*session(), [&](const auto& entry) { return entry.first.starts_with(prefix); });
        }
        graph = &partitionGraph(design(), top);
        const std::string configuration = std::to_string(options_.enableGuardMinSize);
        const bool rebuild = stage(*graph) == "sink-partitioned";
        if (rebuild && graph->metadata("grhsim.sinkConfig") != std::optional<AttributeValue>(configuration))
            throw std::invalid_argument("Changed sink configuration requires the nonsink-partitioned checkpoint");
        if (!rebuild && stage(*graph) != "nonsink-partitioned")
            throw std::invalid_argument("Sink partitioning requires nonsink-partitioned stage");
        if (!session()) throw std::invalid_argument("Sink partitioning requires a session");
        if (!rebuild) {
            const auto writers = orderedWriters(*graph);
            std::unordered_map<OperationId, uint32_t, OperationIdHash> priorities;
            std::unordered_map<OperationId, uint32_t, OperationIdHash> nextPriority;
            std::unordered_map<OperationId, std::string, OperationIdHash> clocks;
            std::unordered_map<ValueId, std::string, ValueIdHash> clockCache;
            for (const auto writer : writers) {
                const auto args = graph->opOperands(writer);
                const bool latch = graph->opKind(writer) == kLatchWrite;
                const auto fire = latch ? ValueId{} : args[0];
                const auto [clock, fresh] = clockCache.try_emplace(fire);
                if (fresh) clock->second = clockSignature(*graph, fire);
                priorities[writer] = nextPriority[writerTarget(*graph, writer)]++;
                clocks[writer] = clock->second;
                // Counts are keyed by a textual value symbol, not ValueId's
                // ordering (ValueId deliberately has equality only).
            }
            std::map<std::pair<std::string, std::string>, uint32_t> counts;
            for (const auto writer : writers) {
                const auto args = graph->opOperands(writer);
                const auto enable = args[graph->opKind(writer) == kLatchWrite ? 0 : 1];
                if (!graph->valueDef(enable) || graph->opKind(graph->valueDef(enable)) != OperationKind::kConstant)
                    ++counts[{clocks.at(writer), std::string(graph->symbolText(graph->valueSymbol(enable)))}];
            }
            std::map<std::string, Owner> groups;
            std::map<Owner, std::unordered_set<OperationId, OperationIdHash>> members;
            std::unordered_map<OperationId, Owner, OperationIdHash> destinations;
            std::unordered_map<OperationId, bool, OperationIdHash> hoisted;
            for (const auto writer : writers) {
                const auto args = graph->opOperands(writer);
                const auto enable = args[graph->opKind(writer) == kLatchWrite ? 0 : 1];
                const auto guard = std::string(graph->symbolText(graph->valueSymbol(enable)));
                const bool hoist = options_.enableGuardMinSize && counts[{clocks.at(writer), guard}] >= options_.enableGuardMinSize;
                const auto category = ownerView(*graph, writer)[1];
                const Owner categoryOwner{"general", category};
                const auto signature = category + "/" + clocks.at(writer) + "/priority=" + std::to_string(priorities.at(writer)) +
                    (hoist ? "/guard=" + std::to_string(guard.size()) + ":" + guard : "/local");
                const auto [it, inserted] = groups.emplace(signature, Owner{});
                if (inserted) it->second = {"general", category, "group_" + std::string(graph->symbolText(graph->operationSymbol(writer)))};
                destinations[writer] = it->second; hoisted[writer] = hoist;
                auto& required = members[it->second];
                required.insert(writer);
                std::vector<OperationId> pending{writer};
                while (!pending.empty()) {
                    const auto op = pending.back(); pending.pop_back();
                    for (const auto value : graph->opOperands(op)) {
                        const auto producer = graph->valueDef(value);
                        if (!producer || ownerView(*graph, producer) != categoryOwner) continue;
                        const auto [existing, fresh] = destinations.emplace(producer, it->second);
                        if (!fresh && existing->second != it->second && !canCloneOperation(*graph, producer))
                            throw std::invalid_argument("Effectful sink cone is shared across clock/priority groups");
                        if (required.insert(producer).second) pending.push_back(producer);
                    }
                }
            }
            for (const auto op : graph->operations()) {
                const auto& path = ownerView(*graph, op);
                if (path.size() == 2 && path[0] == "general" && path[1] != "nonsink" && !destinations.contains(op))
                    throw std::invalid_argument("Orphaned sink computation");
            }
            const auto topo = topological(*graph, [&](OperationId op) { return destinations.contains(op); });
            std::unordered_map<OperationId, std::size_t, OperationIdHash> ranks;
            std::unordered_map<OperationId, std::vector<ValueId>, OperationIdHash> originalOperands;
            for (const auto op : topo) {
                ranks.emplace(op, ranks.size());
                const auto args = graph->opOperands(op);
                originalOperands.emplace(op, std::vector<ValueId>(args.begin(), args.end()));
            }
            result.changed = true;
            for (const auto& [op, key] : destinations) graph->setAttr(op, kPartition, key);
            // Leaf-local CSE before this pass can share pure sink computation
            // across writers. Materialize it in each final clock/priority group.
            for (const auto& [key, required] : members) {
                std::vector<OperationId> ordered(required.begin(), required.end());
                std::sort(ordered.begin(), ordered.end(), [&](auto a, auto b) { return ranks.at(a) < ranks.at(b); });
                std::unordered_map<ValueId, ValueId, ValueIdHash> values;
                for (const auto op : ordered) {
                    auto args = originalOperands.at(op);
                    for (auto& value : args) if (const auto found = values.find(value); found != values.end()) value = found->second;
                    const auto original = graph->opResults(op);
                    const std::vector<ValueId> results(original.begin(), original.end());
                    OperationId target = op;
                    if (destinations.at(op) != key) target = cloneOperation(*graph, op, args, key);
                    else for (std::size_t i = 0; i < args.size(); ++i)
                        if (args[i] != graph->opOperands(op)[i]) graph->replaceOperand(op, i, args[i]);
                    const auto copied = graph->opResults(target);
                    for (std::size_t i = 0; i < results.size(); ++i) values.emplace(results[i], copied[i]);
                }
            }
            for (const auto& [op, hoist] : hoisted) graph->setAttr(op, "grhsim.hoistEnable", hoist);
            graph->setMetadata("grhsim.sinkConfig", configuration);
            graph->setMetadata(kStage, std::string("sink-partitioned"));
            verifyPartitionResult(*graph);
        }
        auto schedule = buildSchedule(*graph);
        session()->insert_or_assign(sinkScheduleKey(*graph),
            std::make_unique<SessionSlotValue<SinkSchedule>>(std::move(schedule)));
    } catch (const std::exception& exception) {
        if (graph && result.changed) graph->setMetadata(kStage, std::string("sink-partitioned-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{}); result.failed = true;
    }
    return result;
}

}
