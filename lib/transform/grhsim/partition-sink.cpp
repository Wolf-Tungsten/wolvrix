#include "transform/grhsim/partition-sink.hpp"

#include "partition-internal.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
using namespace detail;

class SinkPhaseTiming {
public:
    explicit SinkPhaseTiming(const char* scope) : scope_(scope) {}

    void mark(const char* phase, std::size_t writers = 0, std::size_t groups = 0,
              std::size_t runs = 0, std::size_t roots = 0, std::size_t nodes = 0,
              std::size_t edges = 0, std::size_t clones = 0, std::size_t memberships = 0) {
        if (!enabled_) return;
        uint64_t rss = 0, peak = 0;
        if (auto* status = std::fopen("/proc/self/status", "r")) {
            char line[256];
            unsigned long long value;
            while (std::fgets(line, sizeof(line), status)) {
                if (std::sscanf(line, "VmRSS: %llu kB", &value) == 1) rss = value;
                if (std::sscanf(line, "VmHWM: %llu kB", &value) == 1) peak = value;
            }
            std::fclose(status);
        }
        const auto now = std::chrono::steady_clock::now();
        const auto ms = [&](auto from) {
            return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - from).count());
        };
        std::fprintf(stderr, "[grhsim-partition-sink-phase] scope=%s phase=%s elapsed_ms=%llu total_ms=%llu rss_kib=%llu peak_rss_kib=%llu writers=%zu groups=%zu runs=%zu roots=%zu dag_nodes=%zu dag_edges=%zu clones=%zu memberships=%zu\n",
            scope_, phase, ms(previous_), ms(start_), static_cast<unsigned long long>(rss),
            static_cast<unsigned long long>(peak), writers, groups, runs, roots, nodes, edges, clones, memberships);
        previous_ = now;
    }

private:
    const char* scope_;
    const bool enabled_ = std::getenv("WOLVRIX_SIMPLIFY_PHASE_TIMING") != nullptr;
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point previous_ = start_;
};

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

bool isNonsink(StringListView path) {
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
    std::vector<StringListView> order;
    std::vector<std::size_t> prerequisiteOffsets;
    std::vector<uint32_t> prerequisites;
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
        return ownerLess(ownerView(graph, left), ownerView(graph, right));
    });
    std::vector<StringListView> keys;
    for (const auto op : operations) {
        const auto key = ownerView(graph, op);
        if (keys.empty() || !sameOwner(keys.back(), key)) keys.push_back(key);
        result.operationNode[op.index] = keys.size() - 1;
    }
    if (keys.size() > UINT32_MAX) throw std::invalid_argument("Too many nonsink schedule leaves");
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
    for (const auto& [source, target] : edges)
        result.prerequisites[cursor[ordinal[target]]++] = static_cast<uint32_t>(ordinal[source]);
    for (const auto op : operations) result.operationNode[op.index] = ordinal[result.operationNode[op.index]];
    return result;
}

std::vector<uint32_t> directDependencies(const Graph& graph, OperationId writer, const NonsinkDag& dag) {
    std::vector<uint32_t> result;
    std::vector<OperationId> queue{writer};
    std::unordered_set<OperationId, OperationIdHash> seen;
    while (!queue.empty()) {
        const auto op = queue.back(); queue.pop_back();
        if (!seen.insert(op).second) continue;
        const auto node = dag.operationNode[op.index];
        if (node != SIZE_MAX) { result.push_back(static_cast<uint32_t>(node)); continue; }
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
    SinkPhaseTiming timing("schedule");
    timing.mark("start");
    SinkSchedule schedule;
    schedule.stamp = DesignStamp::capture(graph);
    schedule.partitionConfigHash = configHash(graph);
    const auto writers = orderedWriters(graph);
    timing.mark("writers-ordered", writers.size());
    auto nonsinks = nonsinkDag(graph);
    timing.mark("nonsink-dag", writers.size(), 0, 0, 0, nonsinks.order.size(), nonsinks.prerequisites.size());
    schedule.nonsinkKeys.reserve(nonsinks.order.size());
    for (const auto key : nonsinks.order) schedule.nonsinkKeys.push_back(copyOwner(key));
    std::map<PartitionKey, SinkGroup, OwnerLess> groups;
    std::map<PartitionKey, std::string> clocks;
    std::map<PartitionKey, uint32_t> groupPriorities;
    std::map<PartitionKey, bool> groupHoisting;
    std::unordered_map<OperationId, uint32_t, OperationIdHash> priorities;
    std::unordered_map<ValueId, std::string, ValueIdHash> clockCache;
    for (const auto writer : writers) {
        const auto key = copyOwner(ownerView(graph, writer));
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
        }
        auto& run = schedule.generalRuns.back();
        run.orderedWriters.push_back(writer);
        const auto required = directDependencies(graph, writer, nonsinks);
        run.directNonsinkRoots.insert(run.directNonsinkRoots.end(), required.begin(), required.end());
        if (key[1] == "large-sink") {
            if (schedule.largeCommitRuns.empty() || schedule.largeCommitRuns.back().partitionKey != key)
                schedule.largeCommitRuns.push_back({key, {}, {}});
            schedule.largeCommitRuns.back().orderedWriters.push_back(writer);
        }
    }
    // Keep the shared DAG and canonical roots. Materializing every run's full
    // ancestor closure duplicates the same leaf keys across interleaved runs.
    std::size_t rootCount = 0;
    for (auto& run : schedule.generalRuns) {
        auto& roots = run.directNonsinkRoots;
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        rootCount += roots.size();
    }
    schedule.nonsinkPrerequisiteOffsets = std::move(nonsinks.prerequisiteOffsets);
    schedule.nonsinkPrerequisites = std::move(nonsinks.prerequisites);
    timing.mark("run-roots", writers.size(), groups.size(), schedule.generalRuns.size(), rootCount,
                schedule.nonsinkKeys.size(), schedule.nonsinkPrerequisites.size());
    for (const auto op : graph.operations()) {
        const auto& path = ownerView(graph, op);
        if (path.size() == 3 && path[0] == "general" && path[1] != "nonsink" && !groups.contains(path))
            throw std::invalid_argument("Orphaned sink group");
    }
    for (auto& [key, group] : groups) schedule.sinkGroups.push_back(std::move(group));
    schedule.complete = true;
    timing.mark("done", writers.size(), schedule.sinkGroups.size(), schedule.generalRuns.size(), rootCount,
                schedule.nonsinkKeys.size(), schedule.nonsinkPrerequisites.size());
    return schedule;
}
}

std::vector<uint32_t> requiredNonsinkOrdinals(const SinkSchedule& schedule, const SinkRun& run) {
    const auto nodes = schedule.nonsinkKeys.size();
    const auto& offsets = schedule.nonsinkPrerequisiteOffsets;
    const auto& edges = schedule.nonsinkPrerequisites;
    if (nodes > UINT32_MAX || offsets.size() != nodes + 1 || offsets.front() != 0 || offsets.back() != edges.size())
        throw std::invalid_argument("Invalid nonsink schedule DAG");
    if (!std::is_sorted(run.directNonsinkRoots.begin(), run.directNonsinkRoots.end()) ||
        std::adjacent_find(run.directNonsinkRoots.begin(), run.directNonsinkRoots.end()) != run.directNonsinkRoots.end())
        throw std::invalid_argument("Noncanonical nonsink schedule roots");
    std::vector<uint8_t> seen(nodes);
    std::vector<uint32_t> pending;
    const auto push = [&](uint32_t key) {
        if (key >= nodes) throw std::invalid_argument("Nonsink schedule root is out of range");
        if (!seen[key]) { seen[key] = 1; pending.push_back(key); }
    };
    for (const auto key : run.directNonsinkRoots) push(key);
    std::vector<uint32_t> required;
    while (!pending.empty()) {
        const auto key = pending.back(); pending.pop_back();
        required.push_back(key);
        if (offsets[key] > offsets[key + 1] || offsets[key + 1] > edges.size())
            throw std::invalid_argument("Invalid nonsink schedule row");
        for (auto i = offsets[key]; i < offsets[key + 1]; ++i) {
            if (edges[i] >= key) throw std::invalid_argument("Nonsink schedule prerequisite is not topological");
            push(edges[i]);
        }
    }
    std::sort(required.begin(), required.end());
    return required;
}

std::vector<PartitionKey> requiredNonsinkKeys(const SinkSchedule& schedule, const SinkRun& run) {
    std::vector<PartitionKey> result;
    for (const auto ordinal : requiredNonsinkOrdinals(schedule, run)) result.push_back(schedule.nonsinkKeys[ordinal]);
    return result;
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
        if (schedule.partitionConfigHash != expected.partitionConfigHash || schedule.nonsinkKeys != expected.nonsinkKeys ||
            schedule.nonsinkPrerequisiteOffsets != expected.nonsinkPrerequisiteOffsets ||
            schedule.nonsinkPrerequisites != expected.nonsinkPrerequisites || schedule.sinkGroups != expected.sinkGroups ||
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
    SinkPhaseTiming timing("partition");
    timing.mark("start");
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
        timing.mark("input-verified");
        const std::string configuration = std::to_string(options_.enableGuardMinSize);
        const bool rebuild = stage(*graph) == "sink-partitioned";
        if (rebuild && graph->metadata("grhsim.sinkConfig") != std::optional<AttributeValue>(configuration))
            throw std::invalid_argument("Changed sink configuration requires the nonsink-partitioned checkpoint");
        if (!rebuild && stage(*graph) != "nonsink-partitioned")
            throw std::invalid_argument("Sink partitioning requires nonsink-partitioned stage");
        if (!session()) throw std::invalid_argument("Sink partitioning requires a session");
        if (!rebuild) {
            const auto writers = orderedWriters(*graph);
            timing.mark("writers-ordered", writers.size());
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
            timing.mark("clocks", writers.size(), clockCache.size());
            std::map<std::pair<std::string, std::string>, uint32_t> counts;
            for (const auto writer : writers) {
                const auto args = graph->opOperands(writer);
                const auto enable = args[graph->opKind(writer) == kLatchWrite ? 0 : 1];
                if (!graph->valueDef(enable) || graph->opKind(graph->valueDef(enable)) != OperationKind::kConstant)
                    ++counts[{clocks.at(writer), std::string(graph->symbolText(graph->valueSymbol(enable)))}];
            }
            timing.mark("guards-counted", writers.size(), counts.size());
            std::map<std::string, Owner> groups;
            std::map<Owner, std::unordered_set<OperationId, OperationIdHash>> members;
            std::unordered_map<OperationId, Owner, OperationIdHash> destinations;
            std::unordered_map<OperationId, bool, OperationIdHash> hoisted;
            for (const auto writer : writers) {
                const auto args = graph->opOperands(writer);
                const auto enable = args[graph->opKind(writer) == kLatchWrite ? 0 : 1];
                const auto guard = std::string(graph->symbolText(graph->valueSymbol(enable)));
                const bool hoist = options_.enableGuardMinSize && counts[{clocks.at(writer), guard}] >= options_.enableGuardMinSize;
                const auto category = std::string(ownerView(*graph, writer)[1]);
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
                        if (!producer || !sameOwner(ownerView(*graph, producer), categoryOwner)) continue;
                        const auto [existing, fresh] = destinations.emplace(producer, it->second);
                        if (!fresh && existing->second != it->second && !canCloneOperation(*graph, producer))
                            throw std::invalid_argument("Effectful sink cone is shared across clock/priority groups");
                        if (required.insert(producer).second) pending.push_back(producer);
                    }
                }
            }
            std::size_t membershipCount = 0;
            for (const auto& [key, required] : members) membershipCount += required.size();
            timing.mark("sink-cones", writers.size(), groups.size(), 0, 0, 0, 0, 0, membershipCount);
            for (const auto op : graph->operations()) {
                const auto& path = ownerView(*graph, op);
                if (path.size() == 2 && path[0] == "general" && path[1] != "nonsink" && !destinations.contains(op))
                    throw std::invalid_argument("Orphaned sink computation: " +
                        std::string(graph->symbolText(graph->operationSymbol(op))));
            }
            const auto topo = topological(*graph, [&](OperationId op) { return destinations.contains(op); });
            std::unordered_map<OperationId, std::size_t, OperationIdHash> ranks;
            std::unordered_map<OperationId, std::vector<ValueId>, OperationIdHash> originalOperands;
            for (const auto op : topo) {
                ranks.emplace(op, ranks.size());
                const auto args = graph->opOperands(op);
                originalOperands.emplace(op, std::vector<ValueId>(args.begin(), args.end()));
            }
            timing.mark("sink-topological", writers.size(), groups.size(), 0, 0, 0, 0, 0, membershipCount);
            result.changed = true;
            for (const auto& [op, key] : destinations) graph->setAttr(op, kPartition, key);
            // Leaf-local CSE before this pass can share pure sink computation
            // across writers. Materialize it in each final clock/priority group.
            std::size_t clones = 0;
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
                    if (destinations.at(op) != key) { target = cloneOperation(*graph, op, args, key); ++clones; }
                    else for (std::size_t i = 0; i < args.size(); ++i)
                        if (args[i] != graph->opOperands(op)[i]) graph->replaceOperand(op, i, args[i]);
                    const auto copied = graph->opResults(target);
                    for (std::size_t i = 0; i < results.size(); ++i) values.emplace(results[i], copied[i]);
                }
            }
            timing.mark("sink-clones", writers.size(), groups.size(), 0, 0, 0, 0, clones, membershipCount);
            for (const auto& [op, hoist] : hoisted) graph->setAttr(op, "grhsim.hoistEnable", hoist);
            graph->setMetadata("grhsim.sinkConfig", configuration);
            graph->setMetadata(kStage, std::string("sink-partitioned"));
            verifyPartitionResult(*graph);
            timing.mark("partition-verified", writers.size(), groups.size(), 0, 0, 0, 0, clones, membershipCount);
        }
        ingest::node::recordRetention(*graph);
        timing.mark("retention-recorded");
        auto schedule = buildSchedule(*graph);
        session()->insert_or_assign(sinkScheduleKey(*graph),
            std::make_unique<SessionSlotValue<SinkSchedule>>(std::move(schedule)));
        timing.mark("done");
    } catch (const std::exception& exception) {
        timing.mark("failed");
        if (graph && result.changed) graph->setMetadata(kStage, std::string("sink-partitioned-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{}); result.failed = true;
    }
    return result;
}

}
