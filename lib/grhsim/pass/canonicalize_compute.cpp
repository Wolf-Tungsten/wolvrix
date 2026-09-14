#include "grhsim/pass/canonicalize_compute.hpp"
#include "grhsim/ir/model.hpp"

#include <unordered_map>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        bool parameterKey(std::span<const Parameter> parameters, std::string &key)
        {
            for (const auto &parameter : parameters)
            {
                key += std::to_string(parameter.name.index) + ':' + std::to_string(parameter.value.index()) + ':';
                if (const auto *integer = std::get_if<int64_t>(&parameter.value)) key += std::to_string(*integer) + ';';
                else if (const auto *boolean = std::get_if<bool>(&parameter.value)) key += *boolean ? "1;" : "0;";
                else if (const auto *text = std::get_if<std::string>(&parameter.value))
                    key += std::to_string(text->size()) + ':' + *text + ';';
                else if (const auto *strings = std::get_if<std::vector<std::string>>(&parameter.value))
                {
                    key += std::to_string(strings->size()) + '[';
                    for (const auto &text : *strings) key += std::to_string(text.size()) + ':' + text + ';';
                    key += ']';
                }
                else return false;
            }
            return true;
        }

        bool shareEquivalentStates(GrhSimModel &model, diag::Diagnostics &diagnostics)
        {
            std::vector<uint32_t> references(model.states().size() + 1), allowed(references.size()), writers(references.size());
            std::vector<std::string> initial(references.size());
            for (auto ref : model.objectRefPool())
                if (ref.kind == ObjectKind::State) ++references[ref.index];
            for (const auto &record : model.initRecords())
            {
                const auto steps = model.steps(record);
                if (steps.size() != 1 || model.text(steps[0].kind) != "core.init.const") continue;
                auto key = std::to_string(model.states()[record.state.index - 1].type.index) + ':';
                if (parameterKey(model.parameters(steps[0]), key)) initial[record.state.index] = std::move(key);
            }
            for (const auto &op : model.operations())
            {
                const auto name = model.text(op.opType);
                const auto refs = model.objectRefs(op);
                if (name == "core.state.read") ++allowed[refs[0].index];
                else if (name == "core.state.regWrite" || name == "core.state.latchWrite")
                { ++allowed[refs[0].index]; ++writers[refs[0].index]; }
            }
            std::vector<StateId> canonical(references.size());
            std::vector<uint8_t> sharedTargets(references.size());
            for (const auto &state : model.states()) canonical[state.id.index] = state.id;
            std::vector<uint8_t> removeOps(model.operations().size() + 1), removeStates(references.size());
            std::unordered_map<std::string, StateId> groups;
            std::size_t shared = 0, histories = 0;
            for (const auto &op : model.operations())
            {
                const auto name = model.text(op.opType);
                if (name != "core.state.regWrite" && name != "core.state.latchWrite") continue;
                const auto refs = model.objectRefs(op);
                const StateId state{refs[0].index, 0};
                const auto &type = model.types()[model.states()[state.index - 1].type.index - 1];
                if (type.kind != TypeKind::Logic || type.domain != LogicDomain::TwoState ||
                    writers[state.index] != 1 || references[state.index] != allowed[state.index] || initial[state.index].empty()) continue;
                std::string key = std::to_string(op.opType.index) + ':' + initial[state.index] + ':';
                for (auto operand : model.operands(op)) key += std::to_string(operand.index) + ',';
                key += ';';
                if (!parameterKey(model.parameters(op), key)) continue;
                bool privateHistory = true;
                for (auto ref : refs.subspan(1))
                {
                    if (ref.kind != ObjectKind::State || references[ref.index] != 1 || initial[ref.index].empty())
                    { privateHistory = false; break; }
                    key += '/' + initial[ref.index];
                }
                if (!privateHistory) continue;
                const auto [entry, inserted] = groups.emplace(std::move(key), state);
                if (inserted) continue;
                canonical[state.index] = entry->second;
                sharedTargets[entry->second.index] = 1;
                removeStates[state.index] = removeOps[op.id.index] = 1;
                ++shared;
                for (auto ref : refs.subspan(1)) { removeStates[ref.index] = 1; ++histories; }
            }
            if (!shared) return false;
            std::vector<ValueId> values(model.values().size() + 1);
            for (const auto &value : model.values()) values[value.id.index] = value.id;
            std::unordered_map<uint64_t, ValueId> reads;
            std::size_t sharedReads = 0;
            for (const auto &op : model.operations())
            {
                if (model.text(op.opType) != "core.state.read" || model.results(op).size() != 1 ||
                    !model.operands(op).empty() || !model.parameters(op).empty()) continue;
                const auto result = model.results(op)[0];
                const auto state = canonical[model.objectRefs(op)[0].index];
                if (!sharedTargets[state.index]) continue;
                const auto type = model.values()[result.index - 1].type;
                const uint64_t key = (uint64_t(state.index) << 32) | type.index;
                const auto [entry, inserted] = reads.emplace(key, result);
                if (inserted) continue;
                values[result.index] = entry->second;
                removeOps[op.id.index] = 1;
                ++sharedReads;
            }
            for (const auto &op : model.operations())
            {
                if (removeOps[op.id.index]) continue;
                const auto args = model.operands(op), results = model.results(op);
                const auto refSpan = model.objectRefs(op);
                const auto paramSpan = model.parameters(op);
                std::vector<ValueId> operands(args.begin(), args.end());
                std::vector<ObjectRef> refs(refSpan.begin(), refSpan.end());
                bool changed = false;
                for (auto &operand : operands)
                    if (values[operand.index] != operand) { operand = values[operand.index]; changed = true; }
                for (auto &ref : refs)
                    if (ref.kind == ObjectKind::State && canonical[ref.index].index != ref.index)
                    { ref = ObjectRef::state(canonical[ref.index]); changed = true; }
                if (!changed) continue;
                const std::vector<ValueId> out(results.begin(), results.end());
                const std::vector<Parameter> params(paramSpan.begin(), paramSpan.end());
                model.replaceOperation(op.id, model.text(op.opType), operands, out, refs, params);
            }
            model.compact(removeOps, removeStates);
            diagnostics.info("equivalent_states_removed=" + std::to_string(shared) +
                " private_histories_removed=" + std::to_string(histories) + " state_reads_shared=" + std::to_string(sharedReads),
                "grhsim.canonicalize-compute");
            return true;
        }

        class CanonicalizeComputePass final : public Pass
        {
        public:
            CanonicalizeComputePass() : Pass("grhsim.canonicalize-compute", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                bool changed = false;
                while (true)
                {
                    changed = canonicalize(model, diagnostics).changed || changed;
                    if (!shareEquivalentStates(model, diagnostics)) break;
                    changed = true;
                }
                return {true, changed, {}};
            }

        private:
            PassResult canonicalize(GrhSimModel &model, diag::Diagnostics &diagnostics)
            {
                std::vector<ValueId> sources(model.values().size() + 1), canonical(sources.size());
                for (const auto &op : model.operations())
                {
                    const auto operands = model.operands(op), results = model.results(op);
                    if (model.text(op.opType) != "core.compute.assign" || operands.size() != 1 || results.size() != 1 ||
                        !model.objectRefs(op).empty() || !model.parameters(op).empty()) continue;
                    const auto typeId = model.values()[results[0].index - 1].type;
                    const auto &type = model.types()[typeId.index - 1];
                    if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                        typeId == model.values()[operands[0].index - 1].type)
                        sources[results[0].index] = operands[0];
                }
                // Resolve chains independent of operation order. A chain reaching
                // an assignment cycle stays intact; never invent a cycle's value.
                std::vector<uint8_t> visiting(sources.size());
                std::vector<ValueId> path;
                for (const auto &value : model.values())
                {
                    if (canonical[value.id.index]) continue;
                    path.clear();
                    auto current = value.id;
                    while (!canonical[current.index] && sources[current.index] && !visiting[current.index])
                    {
                        visiting[current.index] = 1;
                        path.push_back(current);
                        current = sources[current.index];
                    }
                    if (visiting[current.index])
                        for (auto entry : path) canonical[entry.index] = entry;
                    else
                    {
                        const auto root = canonical[current.index] ? canonical[current.index] : current;
                        canonical[current.index] = root;
                        for (auto entry : path) canonical[entry.index] = root;
                    }
                    for (auto entry : path) visiting[entry.index] = 0;
                }
                std::vector<uint8_t> removed(model.operations().size() + 1);
                std::size_t count = 0, uses = 0;
                for (const auto &op : model.operations())
                {
                    const auto results = model.results(op);
                    if (results.size() == 1 && sources[results[0].index] && canonical[results[0].index] != results[0])
                    {
                        removed[op.id.index] = 1;
                        ++count;
                    }
                }
                std::vector<uint32_t> producer(sources.size()), pending(removed.size());
                std::vector<std::vector<uint32_t>> users(removed.size());
                for (const auto &op : model.operations())
                    for (auto value : model.results(op)) producer[value.index] = op.id.index;
                std::vector<uint32_t> ready;
                for (const auto &op : model.operations())
                {
                    if (removed[op.id.index]) continue;
                    for (auto operand : model.operands(op))
                    {
                        users[producer[canonical[operand.index].index]].push_back(op.id.index);
                        ++pending[op.id.index];
                    }
                    if (!pending[op.id.index]) ready.push_back(op.id.index);
                }
                const auto root = [&](ValueId value) {
                    auto current = value;
                    while (canonical[current.index] != current) current = canonical[current.index];
                    while (canonical[value.index] != current)
                    {
                        const auto next = canonical[value.index]; canonical[value.index] = current; value = next;
                    }
                    return current;
                };
                std::unordered_map<std::string, ValueId> expressions;
                std::size_t common = 0;
                for (std::size_t i = 0; i < ready.size(); ++i)
                {
                    const auto &op = model.operations()[ready[i] - 1];
                    const auto results = model.results(op);
                    bool pure = results.size() == 1 && model.text(op.opType).starts_with("core.compute.") &&
                        model.objectRefs(op).empty();
                    if (pure)
                    {
                        const auto type = model.values()[results[0].index - 1].type;
                        const auto &resultType = model.types()[type.index - 1];
                        pure = resultType.kind == TypeKind::Logic && resultType.domain == LogicDomain::TwoState;
                        std::string key = std::to_string(op.opType.index) + ":" + std::to_string(type.index) + ":";
                        for (auto operand : model.operands(op)) key += std::to_string(root(operand).index) + ',';
                        key += ';';
                        for (const auto &parameter : model.parameters(op))
                        {
                            key += std::to_string(parameter.name.index) + ':' + std::to_string(parameter.value.index()) + ':';
                            if (const auto *integer = std::get_if<int64_t>(&parameter.value)) key += std::to_string(*integer) + ';';
                            else if (const auto *boolean = std::get_if<bool>(&parameter.value)) key += *boolean ? "1;" : "0;";
                            else if (const auto *text = std::get_if<std::string>(&parameter.value))
                                key += std::to_string(text->size()) + ':' + *text + ';';
                            else pure = false;
                        }
                        if (pure)
                        {
                            const auto [entry, inserted] = expressions.emplace(std::move(key), results[0]);
                            if (!inserted)
                            {
                                canonical[results[0].index] = entry->second;
                                removed[op.id.index] = 1;
                                ++common;
                            }
                        }
                    }
                    for (auto user : users[op.id.index])
                        if (--pending[user] == 0) ready.push_back(user);
                }
                for (const auto &value : model.values()) canonical[value.id.index] = root(value.id);
                count += common;
                if (count)
                {
                    for (const auto &op : model.operations())
                    {
                        if (removed[op.id.index]) continue;
                        const auto args = model.operands(op);
                        std::vector<ValueId> operands(args.begin(), args.end());
                        bool changed = false;
                        for (auto &operand : operands)
                            if (canonical[operand.index] != operand)
                            {
                                operand = canonical[operand.index];
                                changed = true;
                                ++uses;
                            }
                        if (!changed) continue;
                        const auto resultSpan = model.results(op);
                        const auto refSpan = model.objectRefs(op);
                        const auto paramSpan = model.parameters(op);
                        const std::vector<ValueId> results(resultSpan.begin(), resultSpan.end());
                        const std::vector<ObjectRef> refs(refSpan.begin(), refSpan.end());
                        const std::vector<Parameter> params(paramSpan.begin(), paramSpan.end());
                        model.replaceOperation(op.id, model.text(op.opType), operands, results, refs, params);
                    }
                    model.compact(removed, std::vector<uint8_t>(model.states().size() + 1));
                }
                diagnostics.info("identity_assigns_removed=" + std::to_string(count - common) +
                                 " common_expressions_removed=" + std::to_string(common) +
                                 " rewritten_uses=" + std::to_string(uses), name());
                return {true, count != 0, {}};
            }
        };
    }

    void registerCanonicalizeComputePass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass(
            "grhsim.canonicalize-compute", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) {
                if (!args.empty())
                {
                    factoryError = "grhsim.canonicalize-compute does not accept arguments";
                    return std::unique_ptr<Pass>{};
                }
                return std::unique_ptr<Pass>(std::make_unique<CanonicalizeComputePass>());
            }, error);
    }
}
