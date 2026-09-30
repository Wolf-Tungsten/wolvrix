#include "grhsim/pass/cone_extract.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace wolvrix::lib::grhsim
{

    namespace
    {
        std::vector<OpId> producerTable(const GrhSimModel &model)
        {
            std::vector<OpId> producers(model.values().size() + 1);
            for (const auto &op : model.operations())
                for (const auto result : model.results(op))
                    producers[result.index] = op.id;
            return producers;
        }

        std::string_view phaseSuffix(SimPhase phase) noexcept
        {
            switch (phase)
            {
            case SimPhase::Event: return ".ev";
            case SimPhase::General: return ".gen";
            case SimPhase::Mem: return ".mem";
            case SimPhase::Output: return ".out";
            case SimPhase::None: return "";
            }
            return "";
        }

        // Only side-effect-free cone members may vanish: pure compute ops and
        // the read-only roots (their object ref is a read, not a write).
        bool sweepable(const GrhSimModel &model, const SimOp &op)
        {
            const auto name = model.text(op.opType);
            return name.starts_with("core.compute.") || name == "core.input.read" ||
                   name == "core.state.read" || name == "core.state.memRead";
        }
    } // namespace

    ConeExtraction extractCone(GrhSimModel &model, std::span<const ValueId> sinks,
                               SimPhase phase)
    {
        ConeExtraction extraction;
        const auto producers = producerTable(model);
        int64_t nextSampleId = static_cast<int64_t>(model.operations().size());
        for (const auto &op : model.operations())
            for (const auto &param : model.parameters(op))
            {
                if (model.text(param.name) != "sample_id") continue;
                const auto *sampleId = std::get_if<int64_t>(&param.value);
                if (sampleId && *sampleId > nextSampleId) nextSampleId = *sampleId;
            }
        std::vector<uint8_t> inCone(model.operations().size() + 1, 0);
        std::vector<OpId> stack;
        for (const auto sink : sinks)
        {
            if (sink.generation != 0 || !sink.valid() || sink.index >= producers.size()) continue;
            const auto producer = producers[sink.index];
            if (!producer || inCone[producer.index]) continue;
            inCone[producer.index] = 1;
            stack.push_back(producer);
        }
        while (!stack.empty())
        {
            const auto id = stack.back();
            stack.pop_back();
            extraction.coneOps.push_back(id);
            for (const auto operand : model.operands(model.operations()[id.index - 1]))
            {
                const auto producer = producers[operand.index];
                if (!producer || inCone[producer.index]) continue;
                inCone[producer.index] = 1;
                stack.push_back(producer);
            }
        }
        std::sort(extraction.coneOps.begin(), extraction.coneOps.end(),
                  [](OpId lhs, OpId rhs) { return lhs.index < rhs.index; });

        const std::string suffix(phaseSuffix(phase));
        extraction.oldToNewValues.assign(model.values().size() + 1, ValueId{});
        // Create every clone value up front so operands can be rewired in any
        // op order (model op ids are not topologically sorted).
        for (const auto id : extraction.coneOps)
        {
            const auto &op = model.operations()[id.index - 1];
            for (const auto result : model.results(op))
            {
                const auto value = model.values()[result.index - 1];
                std::string name;
                if (value.name.valid()) name = std::string(model.text(value.name)) + suffix;
                extraction.oldToNewValues[result.index] =
                    model.addValue(value.type, name, value.origin);
            }
        }
        for (const auto id : extraction.coneOps)
        {
            const auto source = model.operations()[id.index - 1];
            std::vector<ValueId> operands;
            operands.reserve(source.operands.count);
            for (const auto operand : model.operands(source))
            {
                const auto mapped = extraction.oldToNewValues[operand.index];
                operands.push_back(mapped.valid() ? mapped : operand);
            }
            std::vector<ValueId> results;
            results.reserve(source.results.count);
            for (const auto result : model.results(source))
                results.push_back(extraction.oldToNewValues[result.index]);
            const std::vector<ObjectRef> refs(model.objectRefs(source).begin(),
                                              model.objectRefs(source).end());
            std::vector<Parameter> params(model.parameters(source).begin(),
                                          model.parameters(source).end());
            const auto opType = std::string(model.text(source.opType));
            const auto randomFunction = opType == "core.system.function" &&
                std::any_of(params.begin(), params.end(), [&](const Parameter &param) {
                    return model.text(param.name) == "name" &&
                           std::get_if<std::string>(&param.value) &&
                           *std::get_if<std::string>(&param.value) == "random";
                });
            if (randomFunction)
            {
                const auto hasSampleId = std::any_of(params.begin(), params.end(), [&](const Parameter &param) {
                    return model.text(param.name) == "sample_id";
                });
                if (!hasSampleId)
                {
                    if (nextSampleId == std::numeric_limits<int64_t>::max())
                        throw std::overflow_error("random sample ID space is exhausted");
                    params.push_back(Parameter{model.intern("sample_id"), ++nextSampleId});
                    const std::vector<ValueId> sourceOperands(model.operands(source).begin(), model.operands(source).end());
                    const std::vector<ValueId> sourceResults(model.results(source).begin(), model.results(source).end());
                    model.replaceOperation(id, opType, sourceOperands, sourceResults, refs, params);
                }
            }
            std::string name;
            if (source.name.valid()) name = std::string(model.text(source.name)) + suffix;
            const auto clone = model.addOperation(opType, operands, results, refs, params,
                                                  name, source.origin);
            model.setOperationPhase(clone, phase);
            extraction.cloneOps.push_back(clone);
        }
        return extraction;
    }

    std::vector<OpId> sweepDeadConeOps(const GrhSimModel &model,
                                       std::span<const OpId> candidateOps)
    {
        const auto producers = producerTable(model);
        // Each op counts once per distinct operand value.
        std::vector<uint32_t> users(model.values().size() + 1, 0);
        for (const auto &op : model.operations())
        {
            std::vector<ValueId> operands(model.operands(op).begin(), model.operands(op).end());
            std::sort(operands.begin(), operands.end(),
                      [](ValueId lhs, ValueId rhs) { return lhs.index < rhs.index; });
            operands.erase(std::unique(operands.begin(), operands.end()), operands.end());
            for (const auto operand : operands) ++users[operand.index];
        }

        std::vector<uint8_t> candidate(model.operations().size() + 1, 0);
        for (const auto id : candidateOps)
            if (id.index < candidate.size()) candidate[id.index] = 1;

        std::vector<uint8_t> removed(model.operations().size() + 1, 0);
        const auto removable = [&](OpId id) {
            if (!candidate[id.index] || removed[id.index]) return false;
            const auto &op = model.operations()[id.index - 1];
            if (!sweepable(model, op)) return false;
            for (const auto result : model.results(op))
                if (users[result.index] != 0) return false;
            return true;
        };

        std::vector<OpId> pending;
        for (const auto id : candidateOps)
            if (removable(id)) pending.push_back(id);
        std::vector<OpId> dead;
        while (!pending.empty())
        {
            const auto id = pending.back();
            pending.pop_back();
            if (!removable(id)) continue;
            removed[id.index] = 1;
            dead.push_back(id);
            std::vector<ValueId> operands(model.operands(model.operations()[id.index - 1]).begin(),
                                          model.operands(model.operations()[id.index - 1]).end());
            std::sort(operands.begin(), operands.end(),
                      [](ValueId lhs, ValueId rhs) { return lhs.index < rhs.index; });
            operands.erase(std::unique(operands.begin(), operands.end()), operands.end());
            for (const auto operand : operands)
            {
                if (users[operand.index] == 0) continue;
                if (--users[operand.index] != 0) continue;
                const auto producer = producers[operand.index];
                if (producer && producer.index < candidate.size() && candidate[producer.index])
                    pending.push_back(producer);
            }
        }
        return dead;
    }

} // namespace wolvrix::lib::grhsim
