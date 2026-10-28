#include "transform/repcut_grhsim_scenarios.hpp"

#include <algorithm>
#include <bit>
#include <deque>
#include <stdexcept>
#include <unordered_set>

namespace wolvrix::lib::transform
{
    namespace
    {
        using grh::OperationKind;

        template <class T> T attr(const grh::Operation &op, std::string_view name, T fallback = {})
        {
            const auto value = op.attr(name);
            if (value) if (const auto *typed = std::get_if<T>(&*value)) return *typed;
            return fallback;
        }

        const char *stateKey(OperationKind kind)
        {
            switch (kind)
            {
            case OperationKind::kRegisterReadPort:
            case OperationKind::kRegisterWritePort: return "regSymbol";
            case OperationKind::kLatchReadPort:
            case OperationKind::kLatchWritePort: return "latchSymbol";
            case OperationKind::kMemoryReadPort:
            case OperationKind::kMemoryWritePort:
            case OperationKind::kMemoryFillPort: return "memSymbol";
            default: return nullptr;
            }
        }

        bool isWrite(OperationKind kind)
        {
            return kind == OperationKind::kRegisterWritePort || kind == OperationKind::kLatchWritePort ||
                   kind == OperationKind::kMemoryWritePort || kind == OperationKind::kMemoryFillPort;
        }

        RepcutScenarioMask delayed(RepcutScenarioMask mask, const RepcutGrhsimScenarios &scenarios)
        {
            auto result = mask & kRepcutUnknownScenario;
            mask &= ~kRepcutUnknownScenario;
            while (mask)
            {
                const auto bit = std::countr_zero(mask);
                mask &= mask - 1;
                const auto next = scenarios.afterCommit[bit];
                result |= next ? next : kRepcutUnknownScenario;
            }
            return result;
        }
    }

    RepcutGrhsimScenarioReport analyzeRepcutGrhsimScenarios(
        const grh::Graph &graph, const RepcutGrhsimScenarios &scenarios)
    {
        RepcutGrhsimScenarioReport report;
        std::size_t valueCount = 0, operationCount = 0;
        for (const auto value : graph.values()) valueCount = std::max(valueCount, std::size_t(value.index) + 1);
        for (const auto id : graph.operations()) operationCount = std::max(operationCount, std::size_t(id.index) + 1);
        report.values.resize(valueCount);
        report.operations.resize(operationCount);
        report.events.resize(operationCount);
        std::vector<bool> guarded(operationCount), queued(operationCount);
        std::unordered_map<std::string, RepcutScenarioMask> stateMasks;
        std::unordered_map<grh::ValueId, std::pair<RepcutGrhsimEventScenarios, bool>, grh::ValueIdHash> roots;
        const auto resolve = [&](grh::ValueId value) {
            if (const auto cached = roots.find(value); cached != roots.end()) return cached->second;
            const auto original = value;
            std::unordered_set<grh::ValueId, grh::ValueIdHash> seen;
            bool inverse = false;
            std::pair<RepcutGrhsimEventScenarios, bool> result{
                {kRepcutUnknownScenario, kRepcutUnknownScenario, kRepcutUnknownScenario}, false};
            while (seen.insert(value).second)
            {
                if (const auto event = scenarios.events.find(value); event != scenarios.events.end())
                {
                    result = {event->second, true};
                    if (inverse) std::swap(result.first.posedge, result.first.negedge);
                    break;
                }
                const auto def = graph.valueDef(value);
                if (!def.valid() || graph.valueWidth(value) != 1) break;
                const auto op = graph.getOperation(def);
                if (op.operands().size() != 1 || graph.valueWidth(op.operands()[0]) != 1 ||
                    (op.kind() != OperationKind::kAssign && op.kind() != OperationKind::kNot &&
                     op.kind() != OperationKind::kLogicNot)) break;
                inverse ^= op.kind() != OperationKind::kAssign;
                value = op.operands()[0];
            }
            roots.emplace(original, result);
            return result;
        };
        for (const auto &[value, mask] : scenarios.inputs)
        {
            (void)mask;
            if (value.graph != graph.id() || !graph.valueIsInput(value))
                throw std::invalid_argument("scenario input must be a primary input of this graph");
        }
        for (const auto &[value, event] : scenarios.events)
        {
            (void)event;
            if (value.graph != graph.id() || graph.valueWidth(value) != 1)
                throw std::invalid_argument("scenario event root must be a one-bit value of this graph");
        }
        for (const auto id : graph.operations())
        {
            const auto op = graph.getOperation(id);
            if (op.kind() == OperationKind::kInstance || op.kind() == OperationKind::kBlackbox)
                throw std::invalid_argument("raw scenario analysis requires a flat graph");
            const auto edges = attr<std::vector<std::string>>(op, "eventEdge");
            RepcutScenarioMask mask = 0;
            if (!edges.empty())
            {
                std::size_t start = op.operands().size();
                switch (op.kind())
                {
                case OperationKind::kRegisterWritePort: start = 3; break;
                case OperationKind::kMemoryWritePort: start = 4; break;
                case OperationKind::kMemoryFillPort: start = 2; break;
                case OperationKind::kDpicCall:
                case OperationKind::kSystemTask:
                    if (op.operands().size() < edges.size()) throw std::invalid_argument("invalid event operand count");
                    start = op.operands().size() - edges.size(); break;
                default: break;
                }
                if (start > op.operands().size() || op.operands().size() - start != edges.size())
                    throw std::invalid_argument("unsupported or invalid raw event operation");
                for (std::size_t i = 0; i < edges.size(); ++i)
                {
                    const auto [event, resolved] = resolve(op.operands()[start+i]);
                    report.unresolvedEventReferences += !resolved;
                    mask |= edges[i] == "posedge" ? event.posedge :
                            edges[i] == "negedge" ? event.negedge : event.anyedge;
                }
                guarded[id.index] = true;
            }
            else if (isWrite(op.kind()))
            {
                mask = kRepcutUnknownScenario;
                guarded[id.index] = true;
            }
            report.events[id.index] = mask;
            if (isWrite(op.kind()))
            {
                const auto key = attr<std::string>(op, stateKey(op.kind()));
                if (key.empty()) throw std::invalid_argument("raw state write has no storage identity");
                stateMasks[key] |= delayed(mask, scenarios);
            }
        }
        std::deque<grh::OperationId> pending;
        const auto enqueue = [&](grh::OperationId id) {
            if (!queued[id.index]) { queued[id.index] = true; pending.push_back(id); }
        };
        for (const auto value : graph.values())
        {
            if (graph.valueIsInput(value))
            {
                const auto input = scenarios.inputs.find(value);
                report.values[value.index] = input == scenarios.inputs.end() ? kRepcutUnknownScenario : input->second;
            }
            else if (!graph.valueDef(value).valid()) report.values[value.index] = kRepcutUnknownScenario;
        }
        for (const auto id : graph.operations()) enqueue(id);
        while (!pending.empty())
        {
            const auto id = pending.front(); pending.pop_front(); queued[id.index] = false;
            ++report.worklistVisits;
            const auto op = graph.getOperation(id);
            const auto kind = op.kind();
            RepcutScenarioMask mask = 0;
            if (guarded[id.index]) mask = report.events[id.index];
            else if (kind != OperationKind::kConstant && kind != OperationKind::kRegister &&
                     kind != OperationKind::kLatch && kind != OperationKind::kMemory && kind != OperationKind::kDpicImport)
            {
                for (const auto operand : op.operands()) mask |= report.values[operand.index];
                if (const auto *key = stateKey(kind)) mask |= stateMasks[attr<std::string>(op, key)];
                if (kind == OperationKind::kDpicCall || kind == OperationKind::kSystemTask ||
                    (kind == OperationKind::kSystemFunction && attr<bool>(op, "hasSideEffects")))
                    mask |= kRepcutUnknownScenario;
            }
            report.operations[id.index] = mask;
            for (const auto result : op.results())
            {
                const auto added = mask & ~report.values[result.index];
                if (!added) continue;
                report.values[result.index] |= added;
                const auto value = graph.getValue(result);
                for (const auto &user : value.users()) enqueue(user.operation);
            }
        }
        return report;
    }
}
