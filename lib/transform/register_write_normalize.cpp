#include "transform/register_write_normalize.hpp"

#include "register_write_internal.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform
{
    namespace
    {
        using namespace write_detail;

        struct Arm
        {
            ValueId data;
            std::vector<Literal> path;
            bool hold = false;
        };

        bool holdsElement(const Graph &graph, ValueId data, const std::string &symbol,
                          uint64_t row, int32_t width)
        {
            data = strip(graph, data);
            const auto def = graph.valueDef(data);
            if (!def.valid() || graph.valueWidth(data) != width) return false;
            const auto op = graph.getOperation(def);
            const auto attr = op.attr("regSymbol");
            const auto *reg = attr ? std::get_if<std::string>(&*attr) : nullptr;
            return op.kind() == OperationKind::kRegisterReadArrayPort &&
                   op.operands().size() == 1 && reg && *reg == symbol &&
                   constant(graph, op.operands()[0]) == row;
        }

        bool collectArms(const Graph &graph, ValueId data, const std::string &symbol,
                         uint64_t row, int32_t width, std::vector<Literal> &path,
                         std::vector<Arm> &arms, unsigned &budget)
        {
            if (budget == 0 || path.size() > 64) return false;
            --budget;
            data = strip(graph, data);
            const auto def = graph.valueDef(data);
            if (def.valid() && graph.opKind(def) == OperationKind::kMux)
            {
                const auto operands = graph.opOperands(def);
                if (operands.size() != 3 || graph.valueWidth(operands[0]) != 1) return false;
                // Copy operands: recursive clients may eventually mutate the graph.
                const auto cond = operands[0], yes = operands[1], no = operands[2];
                path.emplace_back(cond, true);
                if (!collectArms(graph, yes, symbol, row, width, path, arms, budget)) return false;
                path.back().second = false;
                if (!collectArms(graph, no, symbol, row, width, path, arms, budget)) return false;
                path.pop_back();
                return true;
            }
            arms.push_back({data, path, holdsElement(graph, data, symbol, row, width)});
            return arms.size() <= 64;
        }

        struct Payload
        {
            ValueId data, active;
        };

        Payload pruneHolds(Graph &graph, BooleanBuilder &booleans, ValueId data,
                           const std::string &symbol, uint64_t row, int32_t width)
        {
            data = strip(graph, data);
            if (holdsElement(graph, data, symbol, row, width)) return {{}, booleans.literal(false)};
            const auto def = graph.valueDef(data);
            if (!def.valid() || graph.opKind(def) != OperationKind::kMux) return {data, booleans.literal(true)};
            const auto source = graph.getOperation(def);
            const auto cond = source.operands()[0];
            const auto yes = pruneHolds(graph, booleans, source.operands()[1], symbol, row, width);
            const auto no = pruneHolds(graph, booleans, source.operands()[2], symbol, row, width);
            ValueId active;
            if (yes.active == no.active) active = yes.active;
            else if (constant(graph, yes.active) == 1)
                active = booleans.combine(OperationKind::kOr, cond, no.active);
            else if (constant(graph, no.active) == 0)
                active = booleans.combine(OperationKind::kAnd, cond, yes.active);
            else if (constant(graph, yes.active) == 0)
                active = booleans.combine(OperationKind::kAnd, booleans.negate(cond), no.active);
            else if (constant(graph, no.active) == 1)
                active = booleans.combine(OperationKind::kOr, booleans.negate(cond), yes.active);
            else
                active = booleans.combine(OperationKind::kOr,
                    booleans.combine(OperationKind::kAnd, cond, yes.active),
                    booleans.combine(OperationKind::kAnd, booleans.negate(cond), no.active));
            // On a removed branch the new write enable is false, so either
            // surviving payload is a valid don't-care replacement there.
            if (!yes.data.valid()) return {no.data, active};
            if (!no.data.valid() || yes.data == no.data) return {yes.data, active};
            if (yes.data == source.operands()[1] && no.data == source.operands()[2]) return {data, active};
            const auto result = graph.createValue(width, graph.valueSigned(data));
            const auto mux = graph.createOperation(OperationKind::kMux, graph.makeInternalOpSym());
            graph.addOperand(mux, cond);
            graph.addOperand(mux, yes.data);
            graph.addOperand(mux, no.data);
            graph.addResult(mux, result);
            if (source.srcLoc()) graph.setOpSrcLoc(mux, *source.srcLoc());
            return {result, active};
        }

        std::optional<uint64_t> writeRow(const Graph &graph, const grh::Operation &op)
        {
            if (op.kind() != OperationKind::kRegisterWriteArrayPort &&
                op.kind() != OperationKind::kRegisterWriteArrayMaskPort) return std::nullopt;
            const auto count = grh::arrayPortIndexCount(op);
            if (!count || *count != 1 || op.operands().size() < 2) return std::nullopt;
            return constant(graph, op.operands()[1]);
        }

        bool readsRegister(const Graph &graph, ValueId value, const std::string &symbol,
                           std::unordered_map<ValueId, bool, grh::ValueIdHash> &memo,
                           unsigned &budget, unsigned depth = 0)
        {
            value = strip(graph, value);
            if (const auto it = memo.find(value); it != memo.end()) return it->second;
            if (!budget || depth > 64) return true;
            --budget;
            const auto def = graph.valueDef(value);
            bool reads = false;
            if (def.valid())
            {
                const auto op = graph.getOperation(def);
                const auto attr = op.attr("regSymbol");
                if (const auto *reg = attr ? std::get_if<std::string>(&*attr) : nullptr)
                    reads = *reg == symbol;
                for (auto input : op.operands())
                    if (readsRegister(graph, input, symbol, memo, budget, depth + 1))
                    { reads = true; break; }
            }
            memo.emplace(value, reads);
            return reads;
        }
    }

    RegisterWriteNormalizePass::RegisterWriteNormalizePass(bool twoState)
        : Pass("register-write-normalize", "register-write-normalize",
               "Extract guarded array-element writes and remove proven identity paths"),
          twoState_(twoState)
    {
    }

    PassResult RegisterWriteNormalizePass::run()
    {
        PassResult result;
        if (!twoState_) return result;
        for (const auto &entry : design().graphs())
        {
            auto &graph = *entry.second;
            std::map<std::string, std::vector<OperationId>> groups;
            for (const auto id : graph.operations())
            {
                if (!grh::isRegisterWritePortKind(graph.opKind(id))) continue;
                const auto attr = graph.getOperation(id).attr("regSymbol");
                if (const auto *symbol = attr ? std::get_if<std::string>(&*attr) : nullptr)
                    groups[*symbol].push_back(id);
            }
            BooleanBuilder booleans(graph);
            std::size_t normalized = 0, removedHolds = 0;
            for (const auto &[symbol, writes] : groups)
            {
                // Plan against the complete original write group before erasing
                // anything. Recreate the whole group to preserve declaration
                // priority relative to every unchanged writer.
                std::map<uint32_t, std::vector<Arm>> plans;
                std::unordered_map<ValueId, bool, grh::ValueIdHash> readDependencies;
                std::map<uint64_t, std::vector<std::size_t>> byRow;
                std::vector<std::size_t> unknownRows;
                for (std::size_t pos = 0; pos < writes.size(); ++pos)
                {
                    if (auto row = writeRow(graph, graph.getOperation(writes[pos])))
                        byRow[*row].push_back(pos);
                    else unknownRows.push_back(pos);
                }
                for (std::size_t pos = 0; pos < writes.size(); ++pos)
                {
                    const auto op = graph.getOperation(writes[pos]);
                    if (op.kind() != OperationKind::kRegisterWriteArrayPort ||
                        grh::arrayPortIndexCount(op) != 1 || op.operands().size() < 3) continue;
                    const auto row = constant(graph, op.operands()[1]);
                    if (!row) continue;
                    const auto enable = op.operands()[0], data = op.operands()[2];
                    std::vector<Arm> arms;
                    std::vector<Literal> path;
                    unsigned budget = 256;
                    if (!collectArms(graph, data, symbol, *row, graph.valueWidth(data), path, arms, budget)) continue;
                    if (std::none_of(arms.begin(), arms.end(), [](const Arm &arm) { return arm.hold; })) continue;
                    // Read/modify/write payloads still need the state read and
                    // cannot become sparse overwrite ports. Retain their mux
                    // instead of multiplying per-row arithmetic write paths.
                    bool modifiesOld = false;
                    for (const auto &arm : arms)
                    {
                        if (arm.hold) continue;
                        unsigned dataBudget = 512;
                        if (readsRegister(graph, arm.data, symbol, readDependencies, dataBudget))
                        { modifiesOld = true; break; }
                    }
                    if (modifiesOld) continue;
                    bool safe = true;
                    for (const auto &arm : arms)
                    {
                        if (!arm.hold) continue;
                        auto facts = arm.path;
                        facts.emplace_back(enable, true);
                        const auto checkPrior = [&](const std::vector<std::size_t> &positions) {
                            for (auto before : positions)
                            {
                                if (before >= pos) break;
                                const auto prior = graph.getOperation(writes[before]);
                                if (prior.operands().empty()) continue;
                                facts.emplace_back(prior.operands()[0], true);
                                const bool disjoint = contradictory(graph, facts);
                                facts.pop_back();
                                if (!disjoint) { safe = false; break; }
                            }
                        };
                        checkPrior(byRow[*row]);
                        if (safe) checkPrior(unknownRows);
                        if (!safe) break;
                    }
                    if (safe) plans.emplace(writes[pos].index, std::move(arms));
                }
                if (plans.empty()) continue;
                for (const auto id : writes)
                {
                    const auto op = graph.getOperation(id);
                    std::vector<ValueId> operands(op.operands().begin(), op.operands().end());
                    const auto plan = plans.find(id.index);
                    if (plan == plans.end())
                        copyWrite(graph, op, operands);
                    else
                    {
                        ++normalized;
                        for (const auto &arm : plan->second)
                            if (arm.hold) ++removedHolds;
                        const auto payload = pruneHolds(graph, booleans, op.operands()[2], symbol,
                            *constant(graph, op.operands()[1]), graph.valueWidth(op.operands()[2]));
                        if (payload.data.valid())
                        {
                            operands[0] = booleans.combine(OperationKind::kAnd, op.operands()[0], payload.active);
                            operands[2] = payload.data;
                            copyWrite(graph, op, operands);
                        }
                    }
                }
                for (const auto id : writes) graph.eraseOp(id);
                result.changed = true;
            }
            if (normalized)
                logInfo("register-write-normalize [" + graph.symbol() + "] writes=" +
                        std::to_string(normalized) + " holdPaths=" + std::to_string(removedHolds));
        }
        return result;
    }
}
