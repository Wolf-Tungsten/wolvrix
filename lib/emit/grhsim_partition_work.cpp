#include "emit/grhsim_partition_work.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace wolvrix::lib::emit
{
    GrhSimPartitionWork summarizeGrhSimPartitionWork(
        uint32_t partition, const grh::Graph &graph, const GrhSimExecutionPlan &plan,
        const GrhSimActivityReport &activity,
        std::span<const GrhSimInputBinding> bindings)
    {
        if (plan.graph != graph.id()) throw std::invalid_argument("partition work graph/plan mismatch");
        GrhSimPartitionWork result;
        result.partition = partition;
        result.graph = graph.id();
        result.work = attributeGrhSimWork(plan, activity);
        using Binding = GrhSimInputBinding;
        std::unordered_map<grh::ValueId, Binding, grh::ValueIdHash> byInput;
        for (const auto &binding : bindings)
        {
            if (!binding.input.valid() || binding.input.graph != graph.id() ||
                !graph.valueIsInput(binding.input) || !byInput.emplace(binding.input, binding).second)
                throw std::invalid_argument("invalid/duplicate partition input binding");
            const bool remote = binding.kind == Binding::Kind::DeferredState || binding.kind == Binding::Kind::EarlyEffect;
            if (remote != binding.writerPartition.has_value() ||
                (remote && *binding.writerPartition == partition))
                throw std::invalid_argument("invalid partition input writer");
        }
        for (const auto &input : plan.inputs)
        {
            const auto it = byInput.find(input.value);
            GrhSimPartitionWork::InputConsumer consumer;
            consumer.binding = it == byInput.end() ? Binding{input.value} : it->second;
            consumer.width = graph.valueWidth(input.value);
            consumer.arrivals = activity.values.at(input.value.index);
            consumer.activation = input;
            const auto value = graph.getValue(input.value);
            for (const auto &user : value.users())
                consumer.directUsers.push_back(user.operation);
            std::sort(consumer.directUsers.begin(), consumer.directUsers.end(),
                      [](auto a, auto b) { return a.index < b.index; });
            consumer.directUsers.erase(std::unique(consumer.directUsers.begin(), consumer.directUsers.end()),
                                       consumer.directUsers.end());
            result.inputs.push_back(std::move(consumer));
            byInput.erase(input.value);
        }
        if (!byInput.empty()) throw std::invalid_argument("partition input absent from execution plan");

        using Site = GrhSimAttributedWorkReport::Site;
        using BodyKey = std::tuple<Site::Kind, uint32_t, std::vector<uint32_t>>;
        std::map<BodyKey, std::size_t> bodySites;
        const auto key = [](Site::Kind kind, uint32_t supernode, const auto &operations) {
            std::vector<uint32_t> ids;
            for (auto op : operations) ids.push_back(op.index);
            return BodyKey{kind, supernode, std::move(ids)};
        };
        for (std::size_t index = 0; index < result.work.sites.size(); ++index)
        {
            const auto &site = result.work.sites[index];
            if (site.kind == Site::Kind::EnabledBody || site.kind == Site::Kind::ChangedBody)
            {
                if (!bodySites.emplace(key(site.kind, site.supernode, site.operations), index).second)
                    throw std::invalid_argument("duplicate partition write body site");
            }
            if (site.kind != Site::Kind::Compute) continue;
            for (const auto operation : site.operations)
            {
                using Kind = grh::OperationKind;
                using Source = GrhSimPartitionWork::SourceRead;
                std::optional<Source::Kind> kind;
                switch (graph.getOperation(operation).kind())
                {
                case Kind::kConstant: kind = Source::Kind::Constant; break;
                case Kind::kRegisterReadPort:
                case Kind::kLatchReadPort: kind = Source::Kind::LocalState; break;
                case Kind::kMemoryReadPort: kind = Source::Kind::LocalMemory; break;
                default: break;
                }
                if (kind) result.sources.push_back({*kind, index, operation});
            }
        }
        for (const auto &run : plan.commitWriteRuns)
            for (const auto &body : run.bodies)
            {
                GrhSimPartitionWork::WriteBody entry{body.kind, run.supernode, body.operations, {}, {}};
                const auto find = [&](Site::Kind kind, const auto &features) -> std::optional<std::size_t> {
                    if (features.empty()) return std::nullopt;
                    const auto it = bodySites.find(key(kind, run.supernode, body.operations));
                    if (it == bodySites.end()) throw std::invalid_argument("partition write body lost feature provenance");
                    const auto index = it->second;
                    bodySites.erase(it);
                    return index;
                };
                entry.enabledSite = find(Site::Kind::EnabledBody, body.enabledWork);
                entry.changedSite = find(Site::Kind::ChangedBody, body.changedWork);
                result.bodies.push_back(std::move(entry));
            }
        if (!bodySites.empty()) throw std::invalid_argument("orphan partition write body site");
        return result;
    }
}
