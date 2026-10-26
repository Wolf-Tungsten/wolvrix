#include "transform/grhsim/split-event-and-output.hpp"

#include "partition-internal.hpp"

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
using namespace detail;

bool timeslotTask(const Graph& graph, OperationId op) {
    if (graph.opKind(op) != kSystemTask) return false;
    const auto name = attribute<std::string>(graph, op, "name").value();
    return name == "monitor" || name == "strobe";
}

// Build semantic history in GRH. All old history reads happen before the
// output-end writes; these writes never enter the General fixed point.
void buildChangeHistory(Graph& graph, OperationId task) {
    const std::vector<ValueId> operands(graph.opOperands(task).begin(), graph.opOperands(task).end());
    const LogicalType bit{ValueType::Logic, 1, false, LogicDomain::TwoState, {}};
    const auto historyValue = [&](OperationKind kind, std::span<const ValueId> inputs) {
        const auto value = makeOperation(graph, task, kind, bit, inputs);
        graph.setOpSrcLoc(graph.valueDef(value), makeTransformSrcLoc("grhsim-split-event-and-output", "output-history"));
        return value;
    };
    ValueId changed;
    for (std::size_t i = 1; i < operands.size(); ++i) {
        const auto type = graph.valueLogicalType(operands[i]);
        const auto state = graph.createOperation(OperationKind::kRegister);
        setStateLogicalType(graph, state, type);
        setStateInitSpec(graph, state, {{InitKind::Constant, type.kind == ValueType::String ? "" : "0"}});
        graph.setAttr(state, "grhsim.history", std::string("output-end"));
        inheritOrigin(graph, state, {task});
        graph.setOpSrcLoc(state, makeTransformSrcLoc("grhsim-split-event-and-output", "output-history"));
        const auto read = graph.createOperation(OperationKind::kRegisterReadPort);
        const auto previous = graph.createValue(type);
        graph.setAttr(read, "regSymbol", std::string(graph.symbolText(graph.operationSymbol(state))));
        graph.addResult(read, previous);
        graph.setAttr(read, kPartition, Owner{"output"});
        inheritOrigin(graph, read, {task});
        graph.setOpSrcLoc(read, makeTransformSrcLoc("grhsim-split-event-and-output", "output-history"));
        const auto different = historyValue(type.kind == ValueType::Logic ? OperationKind::kCaseNe : OperationKind::kNe,
                                             std::array{operands[i], previous});
        changed = changed ? historyValue(OperationKind::kOr, std::array{changed, different}) : different;
        const auto write = graph.createOperation(kHistoryWrite);
        graph.addOperand(write, operands[i]);
        graph.setAttr(write, "regSymbol", std::string(graph.symbolText(graph.operationSymbol(state))));
        graph.setAttr(write, kOutputEnd, true);
        graph.setAttr(write, kPartition, Owner{"output"});
        inheritOrigin(graph, write, {task});
        graph.setOpSrcLoc(write, makeTransformSrcLoc("grhsim-split-event-and-output", "output-history"));
    }
    const auto condition = historyValue(OperationKind::kLogicAnd, std::array{operands[1], changed});
    graph.replaceOperand(task, 1, condition);
    graph.setAttr(task, kTimeslot, std::string("change"));
}
}

SplitEventAndOutputPass::SplitEventAndOutputPass(SplitEventAndOutputOptions options)
    : Pass("grhsim-split-event-and-output", "grhsim-split-event-and-output", "Extract Event and Output cones on GRH"),
      options_(std::move(options)) {}

bool SplitEventAndOutputPass::supportsDialect(std::string_view dialect) const noexcept {
    return detail::supports(dialect);
}

PassResult SplitEventAndOutputPass::run() {
    PassResult result;
    Graph* graph = nullptr;
    try {
        graph = &partitionGraph(design(), options_.top);
        if (stage(*graph) == "event-output-split") return result;
        if (stage(*graph) != "converted") throw std::invalid_argument("Event/Output extraction requires converted stage");
        const auto include = [&](OperationId op) { return !declaration(*graph, op); };
        auto topo = topological(*graph, include);
        // Rewrites can append an earlier call's pure producer after a later
        // independent call. Prioritize each call's complete dependency cone,
        // so incidental pure-op placement does not reorder external effects.
        std::unordered_map<OperationId, uint64_t, OperationIdHash> urgency;
        uint64_t callOrdinal = 0;
        for (const auto op : topo) urgency.emplace(op, UINT64_MAX);
        for (const auto op : graph->operations())
            if (operationEffect(*graph, op) == OpEffect::External && !timeslotTask(*graph, op)) urgency.at(op) = callOrdinal++;
        for (auto it = topo.rbegin(); it != topo.rend(); ++it)
            for (const auto value : graph->opOperands(*it)) {
                const auto producer = graph->valueDef(value);
                if (const auto found = urgency.find(producer); found != urgency.end()) found->second = std::min(found->second, urgency.at(*it));
            }
        topo = topological(*graph, include, [&](OperationId op) { return urgency.at(op); });
        const std::vector<Port> outputs(graph->outputPorts().begin(), graph->outputPorts().end());
        const std::vector<InoutPort> inouts(graph->inoutPorts().begin(), graph->inoutPorts().end());
        // 0=General, 1=Event, 2=Output. Demand sets are local to this pass.
        std::unordered_map<OperationId, uint8_t, OperationIdHash> demand;
        std::vector<std::pair<OperationId, unsigned>> pending;
        const auto need = [&](ValueId value, unsigned region) {
            if (const auto producer = graph->valueDef(value)) pending.emplace_back(producer, region);
        };
        for (const auto op : topo) {
            if (graph->opKind(op) == kEdgeDetect) pending.emplace_back(op, 1);
            else if (timeslotTask(*graph, op)) pending.emplace_back(op, 2);
            else if (!canCloneOperation(*graph, op)) pending.emplace_back(op, 0);
        }
        for (const auto& port : outputs) need(port.value, 2);
        for (const auto& port : inouts) { need(port.out, 2); need(port.oe, 2); }
        std::unordered_set<uint32_t> anchored;
        for (const auto symbol : graph->declaredSymbols()) anchored.insert(symbol.value);
        for (const auto& group : graph->generateGroups()) for (const auto symbol : group.symbols) anchored.insert(symbol.value);
        // Undemanded declared values remain visible without forcing a duplicate
        // General evaluation when the original can move to Event or Output.
        const auto propagate = [&] {
            while (!pending.empty()) {
                auto [op, region] = pending.back(); pending.pop_back();
                if (declaration(*graph, op)) continue;
                if (graph->opKind(op) == kEdgeDetect) region = 1;
                else if (timeslotTask(*graph, op)) region = 2;
                else if (!canCloneOperation(*graph, op)) region = 0;
                const auto mask = uint8_t(1u << region);
                if (demand[op] & mask) continue;
                demand[op] |= mask;
                for (const auto input : graph->opOperands(op)) need(input, region);
            }
        };
        propagate();
        for (const auto op : topo) if (!demand[op]) {
            bool declared = anchored.contains(graph->operationSymbol(op).value);
            for (const auto value : graph->opResults(op)) declared |= anchored.contains(graph->valueSymbol(value).value);
            if (declared) pending.emplace_back(op, 0);
        }
        propagate();

        result.changed = true;
        int64_t effectOrder = 0;
        for (const auto op : topo)
            if (operationEffect(*graph, op) == OpEffect::External && !timeslotTask(*graph, op))
                graph->setAttr(op, "grhsim.effectOrder", effectOrder++);
        std::unordered_map<ValueId, std::array<ValueId, 3>, ValueIdHash> variants;
        const std::array<Owner, 3> owners{Owner{"general"}, Owner{"event"}, Owner{"output"}};
        const auto resolve = [&](ValueId input, unsigned region) {
            const auto found = variants.find(input);
            if (found == variants.end()) return input; // Top inputs or a later dead op.
            if (found->second[region]) return found->second[region];
            const auto producer = graph->valueDef(input);
            if (graph->opKind(producer) != kEdgeDetect)
                graph->setAttr(producer, kSampleBoundary, true);
            return input; // Explicit event pulse or a held effectful sample.
        };
        std::vector<OperationId> dead;
        for (const auto op : topo) {
            const auto bits = demand[op];
            if (!bits) { dead.push_back(op); continue; }
            const std::vector<ValueId> oldArgs(graph->opOperands(op).begin(), graph->opOperands(op).end());
            const std::vector<ValueId> oldResults(graph->opResults(op).begin(), graph->opResults(op).end());
            const unsigned originalRegion = bits & 1 ? 0 : bits & 2 ? 1 : 2;
            // Clone before editing the original operand vector.
            for (unsigned region = 0; region < 3; ++region) if ((bits & (1u << region)) && region != originalRegion) {
                std::vector<ValueId> args;
                for (const auto input : oldArgs) args.push_back(resolve(input, region));
                const auto clone = cloneOperation(*graph, op, args, owners[region]);
                const auto results = graph->opResults(clone);
                for (std::size_t i = 0; i < results.size(); ++i) variants[oldResults[i]][region] = results[i];
            }
            std::vector<ValueId> args;
            for (const auto input : oldArgs) args.push_back(resolve(input, originalRegion));
            replaceOperands(*graph, op, args);
            graph->setAttr(op, kPartition, owners[originalRegion]);
            for (const auto value : oldResults) variants[value][originalRegion] = value;
        }
        for (const auto& port : outputs) graph->bindOutputPort(port.name, resolve(port.value, 2));
        for (const auto& port : inouts) graph->bindInoutPort(port.name, port.in, resolve(port.out, 2), resolve(port.oe, 2));
        for (auto it = dead.rbegin(); it != dead.rend(); ++it) {
            const std::vector<ValueId> results(graph->opResults(*it).begin(), graph->opResults(*it).end());
            graph->eraseOp(*it);
            for (const auto value : results) graph->eraseValue(value);
        }
        for (const auto op : topo) if (demand[op] && timeslotTask(*graph, op)) {
            if (attribute<bool>(*graph, op, "eventDriven").value()) graph->setAttr(op, kTimeslot, std::string("event"));
            else buildChangeHistory(*graph, op);
        }
        graph->setMetadata("grhsim.effectPolicy", std::string("single-original-call"));
        graph->setMetadata(kStage, std::string("event-output-split"));
        verifyPartitionResult(*graph);
    } catch (const std::exception& exception) {
        if (graph && result.changed) graph->setMetadata(kStage, std::string("event-output-split-failed"));
        error(exception.what(), graph ? graph->symbol() : std::string{});
        result.failed = true;
    }
    return result;
}

}
