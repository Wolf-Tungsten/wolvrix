#include "emit/grhsim_activity_model.hpp"

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace wolvrix::lib::emit
{
    using namespace grh;

    namespace
    {
        std::string_view backendCostFamily(std::string_view name)
        {
            if (name.starts_with("dispatch.")) return "dispatch";
            if (name.starts_with("event.")) return "commit.event";
            if (name.starts_with("enable.")) return "commit.enable";
            if (name.starts_with("enabled.")) return "commit.enabled";
            if (name.starts_with("changed.")) return "commit.changed";
            if (name.starts_with("coverage.")) return "coverage";

            if (name.starts_with("compute.constant.")) return "compute.constant";
            if (name.starts_with("compute.state_read.")) return "compute.state";
            if (name.starts_with("compute.memory_read.")) return "compute.memory";
            if (name.starts_with("compute.side_effect.")) return "compute.side_effect";
            if (name.starts_with("compute.scalar.")) return "compute.scalar";

            // Keep slice/index paths separate from ordinary wordwise helpers:
            // dynamic index normalization and cross-word merge have different
            // runtime control even when they produce the same output words.
            if (name.starts_with("compute.wide.helper.kSlice") ||
                name.starts_with("compute.wide.helper_cross_word") ||
                name.starts_with("compute.wide.helper.index_"))
                return "compute.wide.slice";
            if (name.starts_with("compute.wide.helper.kCountTrailingZeros") ||
                name.starts_with("compute.wide.helper.kCountOnes") ||
                name.starts_with("compute.wide.helper.ctz_"))
                return "wide_scan";
            if (name.starts_with("compute.wide.helper.kReduce") ||
                name.starts_with("compute.wide.helper.reduce_"))
                return "compute.wide.reduce";
            if (name.starts_with("compute.wide.helper.kConcat") ||
                name.starts_with("compute.wide.concat_"))
                return "compute.wide.concat";
            if (name.starts_with("compute.wide.helper.kReplicate") ||
                name.starts_with("compute.wide.helper.replication_"))
                return "compute.wide.replicate";
            if (name.starts_with("compute.wide.helper."))
                return "compute.wide.wordwise";
            if (name.starts_with("compute.wide."))
                return "compute.wide.shape";

            if (name.starts_with("compute.priority_") ||
                name.starts_with("compute.bool_reduce_") ||
                name == "compute.inverted_state_view_operations" ||
                name == "compute.bypassed_operations")
                return "compute.special";
            if (name.starts_with("compute.generic_") ||
                name == "compute.groups" || name == "compute.lowered_operations" ||
                name == "compute.scheduled_operations")
                return "compute.coverage";
            if (name.starts_with("compute.op.")) return "compute.generic";
            return {};
        }

        std::string mechanismUnit(std::string_view name)
        {
            // Dispatch and coverage are kept as separate structural axes.
            if (name == "dispatch.compute_batches") return "dispatch.compute_batch_sites";
            if (name == "dispatch.commit_batches") return "dispatch.commit_batch_sites";
            if (name == "dispatch.compute_words") return "dispatch.compute_word_slots";
            if (name == "dispatch.commit_words") return "dispatch.commit_word_slots";
            if (name == "dispatch.event_check_sites") return "dispatch.event_check_sites";
            if (name.starts_with("coverage.")) return "coverage." + std::string(name.substr(9));

            if (name == "event.inner_check_sites") return "commit.event_check_sites";
            if (name == "enable.dynamic_predicates" ||
                name == "enable.literal_true_groups" ||
                name == "enable.literal_false_groups")
                return "commit.enable_predicate_groups";
            if (name == "enabled.change_or_terms") return "commit.enable_change_or_terms";
            if (name == "enabled.scalar_comparisons") return "commit.enable_scalar_comparisons";
            if (name == "enabled.scalar_next_values") return "commit.enable_scalar_prepares";
            if (name == "enabled.word_comparisons_upper") return "commit.enable_word_comparisons";
            if (name == "enabled.word_next_values") return "commit.enable_word_prepares";
            if (name == "enabled.mask_merge_words") return "commit.enable_mask_merge_words";
            if (name == "enabled.unconditional_word_stores") return "commit.enable_unconditional_word_stores";
            if (name == "enabled.fill_row_iterations") return "commit.enable_fill_row_iterations";
            if (name == "enabled.packed_fill_slice_words") return "commit.enable_packed_fill_slice_words";
            if (name == "enabled.memory_row_addresses") return "commit.enable_memory_row_addresses";
            if (name == "enabled.row_bounds_checks") return "commit.enable_row_bounds_checks";
            if (name == "changed.scalar_store_slots_upper") return "commit.changed_scalar_store_slots";
            if (name == "changed.reader_activation_calls_upper") return "commit.changed_reader_activation_calls";
            if (name == "changed.reader_targets_upper") return "commit.changed_reader_targets";
            if (name == "changed.row_selective_activation_calls_upper")
                return "commit.changed_row_selective_activation_calls";
            if (name == "changed.producer_dirty_bank_or_sites") return "commit.changed_producer_dirty_sites";

            // Constant/state/memory/side-effect shapes are already disjoint
            // families.  Preserve their suffix as a named mechanism axis.
            if (name.starts_with("compute.constant."))
                return "compute.constant." + std::string(name.substr(17));
            if (name.starts_with("compute.state_read."))
                return "compute.state." + std::string(name.substr(19));
            if (name.starts_with("compute.memory_read."))
                return "compute.memory." + std::string(name.substr(20));
            if (name.starts_with("compute.side_effect."))
                return "compute.side_effect." + std::string(name.substr(20));

            if (name.starts_with("compute.scalar."))
                return "compute.scalar." + std::string(name.substr(15));

            // Wide helper features are split by the loop/control mechanism;
            // helper kind remains in the unit name where it is informative.
            if (name == "compute.wide.helper_cross_word_merges" ||
                name == "compute.wide.helper.cross_word_merges")
                return "compute.wide.slice.cross_word_merges";
            if (name == "compute.wide.helper.index_normalizations")
                return "compute.wide.slice.index_normalizations";
            if (name == "compute.wide.helper.ctz_word_iterations")
                return "compute.wide.scan.word_iterations_upper";
            if (name == "compute.wide.helper.reduce_word_iterations")
                return "compute.wide.reduce.word_iterations";
            if (name == "compute.wide.helper.replication_lanes")
                return "compute.wide.replicate.lanes";
            if (name == "compute.wide.helper_operand_words")
                return "compute.wide.helper_input_words";
            if (name == "compute.wide.helper_output_words")
                return "compute.wide.helper_output_words";
            if (name == "compute.wide.helper_word_iterations")
                return "compute.wide.helper_word_iterations";
            if (name.starts_with("compute.wide.helper.kSlice"))
                return "compute.wide.slice.helper_sites." + std::string(name.substr(26));
            if (name.starts_with("compute.wide.helper.kReduce"))
                return "compute.wide.reduce.helper_sites." + std::string(name.substr(27));
            if (name.starts_with("compute.wide.helper.kReplicate"))
                return "compute.wide.replicate.helper_sites." + std::string(name.substr(30));
            if (name.starts_with("compute.wide.helper."))
                return "compute.wide.wordwise.helper_sites." + std::string(name.substr(20));
            if (name.starts_with("compute.wide."))
                return "compute.wide." + std::string(name.substr(13));

            if (name == "compute.generic_direct_state_views")
                return "compute.state.direct_visible_views";
            if (name.starts_with("compute.generic_"))
                return "compute.generic." + std::string(name.substr(16));
            if (name.starts_with("compute.op."))
                return "compute.generic.operation_sites." + std::string(name.substr(11));
            if (name.starts_with("compute.priority_"))
                return "compute.special." + std::string(name.substr(8));
            if (name.starts_with("compute.bool_reduce_"))
                return "compute.special." + std::string(name.substr(8));
            if (name == "compute.inverted_state_view_operations" ||
                name == "compute.bypassed_operations")
                return "compute.special." + std::string(name.substr(8));
            if (name == "compute.groups" || name == "compute.lowered_operations" ||
                name == "compute.scheduled_operations")
                return "coverage." + std::string(name.substr(8));
            return {};
        }

        void addMechanism(std::map<std::string, uint64_t> &units,
                          std::map<std::string, std::map<std::string, uint64_t>> &attribution,
                          std::string_view name, uint64_t count)
        {
            const auto unit = mechanismUnit(name);
            if (unit.empty()) return;
            units[unit] += count;
            attribution[unit][std::string(name)] += count;
        }
    }

    GrhSimBackendCostReport bucketizeGrhSimWork(const GrhSimWorkReport &work)
    {
        GrhSimBackendCostReport report;
        report.staticFeatures = work.staticFeatures;
        for (const auto &[mask, features] : work.conditionalFeatures)
        {
            for (const auto &[name, count] : features)
            {
                const auto family = backendCostFamily(name);
                if (family.empty())
                {
                    report.unbucketedFeatures[mask][name] += count;
                    continue;
                }
                report.conditionalFeatures[mask][std::string(family)][name] += count;
            }
        }
        return report;
    }

    GrhSimMechanismCostReport projectGrhSimMechanismUnits(
        const GrhSimBackendCostReport &backend)
    {
        GrhSimMechanismCostReport report;
        for (const auto &[name, count] : backend.staticFeatures)
        {
            const auto unit = mechanismUnit(name);
            if (unit.empty()) report.unmappedFeatures[0][name] += count;
            else addMechanism(report.staticUnits, report.staticAttribution, name, count);
        }
        for (const auto &[mask, families] : backend.conditionalFeatures)
        {
            for (const auto &[family, features] : families)
            {
                (void)family;
                for (const auto &[name, count] : features)
                {
                    const auto unit = mechanismUnit(name);
                    if (unit.empty()) report.unmappedFeatures[mask][name] += count;
                    else addMechanism(report.conditionalUnits[mask],
                                      report.conditionalAttribution[mask], name, count);
                }
            }
        }
        for (const auto &[mask, features] : backend.unbucketedFeatures)
            for (const auto &[name, count] : features)
                report.unmappedFeatures[mask][name] += count;
        return report;
    }

    static GrhSimWorkReport summarizeWork(const GrhSimExecutionPlan &plan,
                                          const GrhSimActivityReport &activity,
                                          GrhSimAttributedWorkReport *attribution)
    {
        if (plan.activationOverlays || plan.inputFullpass || plan.posedgeFullpass)
            throw std::invalid_argument("work summary requires ordinary dispatch/activation");
        GrhSimWorkReport report;
        using Site = GrhSimAttributedWorkReport::Site;
        const auto record = [&](Site::Kind kind, uint32_t supernode, GrhSimActivityMask mask,
                                const std::vector<OperationId> &operations, std::string_view prefix,
                                const std::map<std::string, uint64_t> &features) {
            if (attribution == nullptr || features.empty()) return;
            if (operations.empty()) throw std::invalid_argument("work site has no operation provenance");
            Site site{kind, supernode, mask, operations, {}};
            for (const auto &[name, count] : features)
                site.features[std::string(prefix) + name] = count;
            attribution->sites.push_back(std::move(site));
        };
        std::unordered_map<OperationId, const GrhSimExecutionPlan::Write *, OperationIdHash> writers;
        for (const auto &write : plan.writes) writers.emplace(write.operation, &write);
        std::map<uint32_t, const GrhSimExecutionPlan::Group *> groups;
        std::map<uint32_t, GrhSimActivityMask> groupEvents;
        for (const auto &group : plan.groups)
        {
            groups.emplace(group.supernode, &group);
            if (group.phase == GrhSimExecutionPlan::Phase::Commit)
                for (auto op : group.operations) groupEvents[group.supernode] |= activity.eventMasks.at(op.index);
        }
        for (const auto &batch : plan.batches)
        {
            const std::string phase = batch.phase == GrhSimExecutionPlan::Phase::Compute ? "compute" : "commit";
            ++report.staticFeatures["dispatch." + phase + "_batches"];
            report.staticFeatures["dispatch." + phase + "_words"] += batch.words.size();
        }
        for (const auto &check : plan.commitEventChecks)
            report.staticFeatures["dispatch.event_check_sites"] += check.expression != "true";
        for (const auto &run : plan.commitWriteRuns)
        {
            if (!run.eventHandledByDispatch && run.eventExpression != "true")
            {
                ++report.conditionalFeatures[groupEvents.at(run.supernode)]["event.inner_check_sites"];
                if (attribution != nullptr)
                {
                    const auto &ops = groups.at(run.supernode)->operations;
                    if (run.begin >= run.end || run.end > ops.size())
                        throw std::invalid_argument("invalid commit run operation range");
                    record(Site::Kind::EventCheck, run.supernode, groupEvents.at(run.supernode),
                           std::vector<OperationId>(ops.begin() + run.begin, ops.begin() + run.end),
                           "event.", {{"inner_check_sites", 1}});
                }
            }
            for (const auto &guard : run.enables)
            {
                if (guard.operations.empty()) throw std::invalid_argument("empty enable group");
                GrhSimActivityMask mask = 0;
                for (auto op : guard.operations) mask |= activity.eventMasks.at(op.index);
                const auto proof = writers.at(guard.operations.front())->enableProof;
                const std::string feature = proof == GrhSimExecutionPlan::Predicate::Unknown
                    ? "enable.dynamic_predicates" : (proof == GrhSimExecutionPlan::Predicate::True
                        ? "enable.literal_true_groups" : "enable.literal_false_groups");
                ++report.conditionalFeatures[mask][feature];
                record(Site::Kind::EnableCheck, run.supernode, mask, guard.operations, "", {{feature, 1}});
            }
            for (const auto &body : run.bodies)
            {
                report.unmodeledCommitBodies += !body.unmodeledWork.empty();
                GrhSimActivityMask enabled = 0, changed = 0;
                for (auto op : body.operations)
                {
                    // A zero mask suppresses possible changes, NOT the event
                    // visit or data/merge/compare work in every lowering path.
                    if (writers.at(op)->enableProof != GrhSimExecutionPlan::Predicate::False)
                        enabled |= activity.eventMasks.at(op.index);
                    changed |= activity.writeMasks.at(op.index);
                }
                for (const auto &[name, count] : body.enabledWork)
                    report.conditionalFeatures[enabled]["enabled." + name] += count;
                for (const auto &[name, count] : body.changedWork)
                    report.conditionalFeatures[changed]["changed." + name] += count;
                record(Site::Kind::EnabledBody, run.supernode, enabled, body.operations, "enabled.", body.enabledWork);
                record(Site::Kind::ChangedBody, run.supernode, changed, body.operations, "changed.", body.changedWork);
            }
        }
        for (const auto &compute : plan.computeLowering)
        {
            report.unmodeledComputeGroups += !compute.unmodeledWork.empty();
            report.genericComputeOperations += compute.genericOperations.size();
            const auto *group = groups.at(compute.supernode);
            const auto mask = activity.computeGroupMasks.at(group->activeId);
            for (const auto &[name, count] : compute.features)
                report.conditionalFeatures[mask]["compute." + name] += count;
            if (attribution != nullptr)
            {
                if (compute.sites.empty())
                {
                    for (const auto &[name, count] : compute.features)
                        attribution->unattributedFeatures[mask]["compute." + name] += count;
                    continue;
                }
                std::map<std::string, uint64_t> totals;
                std::unordered_set<OperationId, OperationIdHash> remaining;
                for (auto op : compute.loweredOperations)
                    if (!remaining.insert(op).second) throw std::invalid_argument("duplicate lowered operation");
                for (auto op : compute.genericOperations)
                    if (!remaining.insert(op).second) throw std::invalid_argument("duplicate generic operation");
                for (const auto &site : compute.sites)
                {
                    for (auto op : site.operations)
                        if (remaining.erase(op) != 1) throw std::invalid_argument("invalid compute site membership");
                    for (const auto &[name, count] : site.features) totals[name] += count;
                    record(Site::Kind::Compute, compute.supernode, mask, site.operations, "compute.", site.features);
                }
                if (!remaining.empty() || totals != compute.features)
                    throw std::invalid_argument("compute site attribution does not conserve group work");
            }
        }
        report.staticFeatures["coverage.unmodeled_commit_writes"] = plan.unmodeledCommitWrites.size();
        report.staticFeatures["coverage.unmodeled_commit_bodies"] = report.unmodeledCommitBodies;
        report.staticFeatures["coverage.unmodeled_compute_groups"] = report.unmodeledComputeGroups;
        report.staticFeatures["coverage.generic_compute_operations"] = report.genericComputeOperations;
        return report;
    }

    GrhSimWorkReport summarizeGrhSimWork(const GrhSimExecutionPlan &plan,
                                       const GrhSimActivityReport &activity)
    {
        return summarizeWork(plan, activity, nullptr);
    }

    GrhSimAttributedWorkReport attributeGrhSimWork(const GrhSimExecutionPlan &plan,
                                                  const GrhSimActivityReport &activity)
    {
        GrhSimAttributedWorkReport result;
        result.summary = summarizeWork(plan, activity, &result);
        return result;
    }

    namespace
    {
        template <typename T> T attr(const Operation &op, std::string_view key, T fallback = {})
        {
            const auto value = op.attr(key);
            if (value) if (const auto *p = std::get_if<T>(&*value)) return *p;
            return fallback;
        }

        void append(GrhSimActivityRanges &ranges, uint32_t begin, uint32_t end, uint64_t mask)
        {
            if (begin >= end || mask == 0) return;
            if (!ranges.empty() && ranges.back().end == begin && ranges.back().mask == mask)
                ranges.back().end = end;
            else ranges.push_back({begin, end, mask});
        }

        GrhSimActivityRanges slice(const GrhSimActivityRanges &ranges, uint32_t begin, uint32_t width)
        {
            GrhSimActivityRanges result;
            for (const auto &r : ranges)
            {
                const uint64_t lo = std::max<uint64_t>(r.begin, begin);
                const uint64_t hi = std::min<uint64_t>(r.end, uint64_t(begin) + width);
                if (lo < hi) append(result, lo - begin, hi - begin, r.mask);
            }
            return result;
        }

        uint32_t width(const Graph &graph, ValueId value)
        {
            // Non-logic values carry one abstract lane, never a fabricated bit width.
            return graph.valueType(value) == ValueType::Logic
                ? static_cast<uint32_t>(std::max<int32_t>(1, graph.valueWidth(value))) : 1;
        }

        const char *stateKey(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kRegisterReadPort: return "regSymbol";
            case OperationKind::kLatchReadPort: return "latchSymbol";
            case OperationKind::kMemoryReadPort: return "memSymbol";
            default: return nullptr;
            }
        }

        GrhSimEventActivity resolveEvent(const Graph &graph, ValueId value,
                                         const GrhSimActivitySeeds &seeds, bool &resolved)
        {
            bool inverted = false;
            std::unordered_set<ValueId, ValueIdHash> seen;
            while (seen.insert(value).second)
            {
                if (const auto it = seeds.events.find(value); it != seeds.events.end())
                {
                    resolved = true;
                    auto event = it->second;
                    if (inverted) std::swap(event.posedge, event.negedge);
                    event.anyedge |= event.posedge | event.negedge;
                    return event;
                }
                if (graph.valueWidth(value) != 1) break;
                const auto id = graph.valueDef(value);
                if (!id.valid()) break;
                const auto op = graph.getOperation(id);
                if (op.operands().size() != 1 || graph.valueWidth(op.operands()[0]) != 1) break;
                if (op.kind() != OperationKind::kAssign && op.kind() != OperationKind::kNot &&
                    op.kind() != OperationKind::kLogicNot) break;
                if (op.kind() != OperationKind::kAssign) inverted = !inverted;
                value = op.operands()[0];
            }
            resolved = false;
            return {kGrhSimUnknownActivity, kGrhSimUnknownActivity, kGrhSimUnknownActivity};
        }
    }

    GrhSimActivityMask grhsimActivityInRange(const GrhSimActivityRanges &ranges, uint32_t begin, uint32_t end)
    {
        GrhSimActivityMask mask = 0;
        for (const auto &r : ranges)
        {
            if (r.begin >= end) break;
            if (r.end > begin) mask |= r.mask;
        }
        return mask;
    }

    GrhSimActivityReport analyzeGrhSimActivity(const Graph &graph, const GrhSimExecutionPlan &plan,
                                              const GrhSimActivitySeeds &seeds, bool preserveBitRanges)
    {
        return analyzeGrhSimActivity(graph, plan, seeds, GrhSimActivityOptions{
            .preserveBitRanges = preserveBitRanges,
            .unseededEvents = GrhSimUnseededEvents::OpaqueControl,
            .honorProvedWriteGuards = false});
    }

    GrhSimActivityReport analyzeGrhSimActivity(const Graph &graph, const GrhSimExecutionPlan &plan,
                                              const GrhSimActivitySeeds &seeds, const GrhSimActivityOptions &options)
    {
        const bool preserveBitRanges = options.preserveBitRanges;
        if (graph.id() != plan.graph) throw std::invalid_argument("activity plan belongs to a different graph");
        if (plan.inputFullpass || plan.posedgeFullpass || plan.activationOverlays)
            throw std::invalid_argument("activity envelope does not model fullpass/activation overlays");
        GrhSimActivityReport report;
        std::size_t valueCount = 1, opCount = 1;
        for (auto id : graph.values()) valueCount = std::max(valueCount, std::size_t(id.index) + 1);
        for (auto id : graph.operations()) opCount = std::max(opCount, std::size_t(id.index) + 1);
        report.values.resize(valueCount);
        report.operationMasks.resize(opCount);
        report.eventMasks.resize(opCount);
        report.writeMasks.resize(opCount);
        std::vector<uint64_t> guards(opCount);
        std::vector<bool> guarded(opCount), queued(opCount);
        std::vector<std::vector<ValueId>> derivedEvents(opCount);
        std::vector<const GrhSimExecutionPlan::Write *> writes(opCount);
        std::unordered_map<std::string, std::vector<OperationId>> stateReaders;
        for (auto id : graph.operations())
        {
            const auto op = graph.getOperation(id);
            if (const auto *key = stateKey(op.kind())) stateReaders[attr<std::string>(op, key)].push_back(id);
        }
        for (const auto &event : plan.events)
        {
            if (event.operation.graph != graph.id() || event.operation.index >= opCount ||
                event.values.size() != event.edges.size()) throw std::invalid_argument("invalid event plan");
            guarded[event.operation.index] = true;
            for (std::size_t i = 0; i < event.values.size(); ++i)
            {
                bool resolved = false;
                const auto e = resolveEvent(graph, event.values[i], seeds, resolved);
                report.unresolvedEvents += !resolved;
                if (!resolved && options.unseededEvents == GrhSimUnseededEvents::SeededRootsOnly)
                {
                    ++report.residualEventReferences;
                    continue;
                }
                if (!resolved && options.unseededEvents == GrhSimUnseededEvents::ValueDependencies)
                {
                    derivedEvents[event.operation.index].push_back(event.values[i]);
                    ++report.derivedEventReferences;
                    continue;
                }
                const auto &edge = event.edges[i];
                guards[event.operation.index] |= edge == "posedge" ? e.posedge
                    : edge == "negedge" ? e.negedge : e.anyedge;
            }
        }
        for (const auto &write : plan.writes)
        {
            if (write.operation.graph != graph.id() || write.operation.index >= opCount)
                throw std::invalid_argument("invalid write plan");
            // Event-less/latch writes remain conservative. Their absence of an
            // edge list is not a proof of inactivity or a zero enable value.
            if (!guarded[write.operation.index]) guards[write.operation.index] = kGrhSimUnknownActivity;
            guarded[write.operation.index] = true;
            writes[write.operation.index] = &write;
            if (options.honorProvedWriteGuards &&
                (write.enableProof == GrhSimExecutionPlan::Predicate::False ||
                 write.maskNonzeroProof == GrhSimExecutionPlan::Predicate::False)) ++report.provedDisabledWrites;
        }
        for (const auto &input : plan.inputs)
        {
            const auto it = seeds.inputs.find(input.value);
            auto &ranges = report.values.at(input.value.index);
            const auto w = width(graph, input.value);
            if (it == seeds.inputs.end()) append(ranges, 0, w, kGrhSimUnknownActivity);
            else
            {
                uint32_t end = 0;
                for (const auto &r : it->second)
                {
                    if (r.begin < end || r.begin >= r.end || r.end > w)
                        throw std::invalid_argument("input activity ranges overlap or exceed value width");
                    append(ranges, r.begin, r.end, r.mask);
                    end = r.end;
                }
            }
            if (!preserveBitRanges)
            {
                const auto mask = grhsimActivityInRange(ranges, 0, w);
                ranges.clear(); append(ranges, 0, w, mask);
            }
        }
        std::deque<OperationId> queue;
        const auto enqueue = [&](OperationId id) {
            if (!queued[id.index]) { queue.push_back(id); queued[id.index] = true; }
        };
        for (auto id : graph.operations()) enqueue(id);
        while (!queue.empty())
        {
            const auto id = queue.front(); queue.pop_front(); queued[id.index] = false;
            ++report.worklistVisits;
            const auto op = graph.getOperation(id);
            const auto kind = op.kind();
            uint64_t mask = 0;
            if (guarded[id.index])
            {
                mask = guards[id.index];
                for (auto value : derivedEvents[id.index])
                    mask |= grhsimActivityInRange(report.values[value.index], 0, width(graph, value));
                report.eventMasks[id.index] = mask;
                if (const auto *write = writes[id.index])
                {
                    if (options.honorProvedWriteGuards &&
                        (write->enableProof == GrhSimExecutionPlan::Predicate::False ||
                         write->maskNonzeroProof == GrhSimExecutionPlan::Predicate::False)) mask = 0;
                    report.writeMasks[id.index] = mask;
                    auto &stateMask = report.stateMasks[write->state];
                    if ((stateMask | mask) != stateMask)
                    {
                        stateMask |= mask;
                        for (auto reader : stateReaders[write->state]) enqueue(reader);
                    }
                }
            }
            else if (kind == OperationKind::kConstant || kind == OperationKind::kRegister ||
                     kind == OperationKind::kLatch || kind == OperationKind::kMemory || kind == OperationKind::kDpicImport) {}
            else
            {
                for (auto v : op.operands()) mask |= grhsimActivityInRange(report.values[v.index], 0, width(graph, v));
                if (const auto *key = stateKey(kind)) mask |= report.stateMasks[attr<std::string>(op, key)];
                if (kind == OperationKind::kSystemFunction || kind == OperationKind::kDpicCall ||
                    kind == OperationKind::kSystemTask) mask |= kGrhSimUnknownActivity;
                if (kind == OperationKind::kInstance || kind == OperationKind::kBlackbox)
                    throw std::invalid_argument("activity analysis requires a flat backend unit");
            }
            // Range-preserving plumbing prevents unrelated bits of a boundary
            // bundle from making every slice appear active in every scenario.
            for (auto result : op.results())
            {
                const auto w = width(graph, result);
                GrhSimActivityRanges ranges;
                if (preserveBitRanges && kind == OperationKind::kConcat)
                {
                    uint64_t offset = 0;
                    for (auto it = op.operands().rbegin(); it != op.operands().rend(); ++it)
                    {
                        for (const auto &r : report.values[it->index])
                            if (offset + r.begin < w)
                                append(ranges, offset + r.begin, std::min<uint64_t>(w, offset + r.end), r.mask);
                        offset += width(graph, *it);
                    }
                }
                else if (preserveBitRanges && kind == OperationKind::kSliceStatic && op.operands().size() == 1)
                {
                    const auto start = attr<int64_t>(op, "sliceStart", -1);
                    if (start >= 0 && uint64_t(start) <= std::numeric_limits<uint32_t>::max())
                        ranges = slice(report.values[op.operands()[0].index], start, w);
                    else append(ranges, 0, w, mask);
                }
                else if (preserveBitRanges && (kind == OperationKind::kAssign || kind == OperationKind::kNot) &&
                         op.operands().size() == 1 && w == width(graph, op.operands()[0]))
                    ranges = report.values[op.operands()[0].index];
                else append(ranges, 0, w, mask);
                // Each transfer function is monotone; the graph may contain
                // cycles, so changes enqueue users until the finite mask lattice settles.
                if (ranges != report.values[result.index])
                {
                    report.values[result.index] = std::move(ranges);
                    const auto value = graph.getValue(result);
                    for (const auto &user : value.users()) enqueue(user.operation);
                }
            }
            if (kind == OperationKind::kSliceStatic && !op.results().empty())
                mask = grhsimActivityInRange(report.values[op.results()[0].index], 0, width(graph, op.results()[0]));
            report.operationMasks[id.index] = mask;
        }
        for (const auto &group : plan.groups)
            report.computeGroupMasks.resize(std::max(report.computeGroupMasks.size(), group.activeId + 1));
        const auto activate = [&](uint32_t target, uint64_t mask) {
            if (target >= report.computeGroupMasks.size()) throw std::invalid_argument("invalid activation target");
            report.computeGroupMasks[target] |= mask;
        };
        for (const auto &input : plan.inputs)
        {
            std::map<uint32_t, uint64_t> selected;
            for (const auto &bits : input.bitMasks)
            {
                auto &mask = selected[bits.target];
                uint64_t word = bits.mask;
                while (word)
                {
                    const auto bit = std::countr_zero(word);
                    const uint64_t pos = bits.word * UINT64_C(64) + bit;
                    if (pos < width(graph, input.value))
                        mask |= grhsimActivityInRange(report.values[input.value.index], pos, pos + 1);
                    word &= word - 1;
                }
            }
            for (auto target : input.targets)
                activate(target, selected.contains(target) ? selected.at(target)
                    : grhsimActivityInRange(report.values[input.value.index], 0, width(graph, input.value)));
        }
        for (const auto &boundary : plan.boundaries)
            for (auto target : boundary.targets)
                activate(target, grhsimActivityInRange(report.values[boundary.value.index], 0, width(graph, boundary.value)));
        for (const auto &state : plan.states)
            for (auto target : state.targets) activate(target, report.stateMasks[state.symbol]);
        for (const auto &events : derivedEvents) for (auto value : events)
            report.derivedEventReferencesWithUnknown +=
                (grhsimActivityInRange(report.values[value.index], 0, width(graph, value)) & kGrhSimUnknownActivity) != 0;
        return report;
    }
}
