#include "grhsim/pass/canonicalize_compute.hpp"

#include "grhsim/ir/model.hpp"
#include "simplify_internal.hpp"

#include "slang/numeric/SVInt.h"

#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        std::optional<uint64_t> scalarConstant(const GrhSimModel &model, const SimOp &op)
        {
            const auto results = model.results(op);
            if (model.text(op.opType) != "core.compute.constant" || results.size() != 1 ||
                !model.operands(op).empty() || !model.objectRefs(op).empty() || model.parameters(op).size() != 1) return {};
            const auto &type = model.types()[model.values()[results[0].index - 1].type.index - 1];
            if (type.kind != TypeKind::Logic || type.domain != LogicDomain::TwoState ||
                type.width == 0 || type.width > 64) return {};
            for (const auto &parameter : model.parameters(op))
            {
                if (model.text(parameter.name) != "constValue" && model.text(parameter.name) != "value") continue;
                std::string literal;
                if (const auto *text = std::get_if<std::string>(&parameter.value)) literal = *text;
                else if (const auto *integer = std::get_if<int64_t>(&parameter.value)) literal = std::to_string(*integer);
                else if (const auto *boolean = std::get_if<bool>(&parameter.value)) literal = *boolean ? "1" : "0";
                else return {};
                try
                {
                    // Match two-state constant semantics: resize with the literal's
                    // signedness, then project X/Z to zero before reading raw bits.
                    auto bits = slang::SVInt::fromString(literal).resize(type.width);
                    bits.flattenUnknowns();
                    return bits.getRawPtr()[0];
                }
                catch (const std::exception &) { return {}; }
            }
            return {};
        }

        ValueId simplify(const GrhSimModel &model, const SimOp &op, std::span<const ValueId> operands,
                         const std::vector<std::optional<uint64_t>> &constants)
        {
            if (!model.parameters(op).empty()) return {};
            const auto result = model.results(op)[0];
            const auto typeId = model.values()[result.index - 1].type;
            const auto &type = model.types()[typeId.index - 1];
            const auto sameType = [&](ValueId value) { return model.values()[value.index - 1].type == typeId; };
            const auto name = model.text(op.opType);
            if (name == "core.compute.mux" && operands.size() == 3 && sameType(operands[1]) && sameType(operands[2]))
            {
                const auto &conditionType = model.types()[model.values()[operands[0].index - 1].type.index - 1];
                if (conditionType.kind != TypeKind::Logic || conditionType.domain != LogicDomain::TwoState) return {};
                if (operands[1] == operands[2]) return operands[1];
                if (const auto condition = constants[operands[0].index]) return *condition ? operands[1] : operands[2];
                return {};
            }
            if (operands.size() != 2 || !sameType(operands[0]) || !sameType(operands[1])) return {};
            if ((name == "core.compute.and" || name == "core.compute.or") && operands[0] == operands[1])
                return operands[0];
            if (type.width == 0 || type.width > 64) return {};
            const auto a = operands[0], b = operands[1];
            const auto lhs = constants[a.index], rhs = constants[b.index];
            const uint64_t mask = type.width == 64 ? UINT64_MAX : (UINT64_C(1) << type.width) - 1;
            if (name == "core.compute.add" || name == "core.compute.sub" || name == "core.compute.xor")
            {
                if (rhs == 0) return a;
                if (name != "core.compute.sub" && lhs == 0) return b;
            }
            else if (name == "core.compute.mul")
            {
                if (lhs == 0 || rhs == 1) return a;
                if (rhs == 0 || lhs == 1) return b;
            }
            else if (name == "core.compute.div")
            {
                if (rhs == 1 && !(type.isSigned && type.width == 1)) return a;
            }
            else if (name == "core.compute.and")
            {
                if (lhs == 0 || rhs == mask) return a;
                if (rhs == 0 || lhs == mask) return b;
            }
            else if (name == "core.compute.or")
            {
                if (lhs == mask || rhs == 0) return a;
                if (rhs == mask || lhs == 0) return b;
            }
            else if (type.width == 1 && (name == "core.compute.logicAnd" || name == "core.compute.logicOr"))
            {
                // Logical results are Boolean; wider integer operands cannot be
                // substituted without an explicit Boolean conversion.
                const uint64_t absorbing = name == "core.compute.logicAnd" ? 0 : 1;
                if (lhs == absorbing || rhs == 1 - absorbing) return a;
                if (rhs == absorbing || lhs == 1 - absorbing) return b;
            }
            return {};
        }

        bool commutative(std::string_view name)
        {
            return name == "core.compute.add" || name == "core.compute.mul" ||
                name == "core.compute.and" || name == "core.compute.or" || name == "core.compute.xor" ||
                name == "core.compute.xnor" || name == "core.compute.eq" || name == "core.compute.ne" ||
                name == "core.compute.caseEq" || name == "core.compute.caseNe" ||
                name == "core.compute.logicAnd" || name == "core.compute.logicOr";
        }

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

        struct CanonicalizeStats
        {
            uint64_t assigns = 0, algebraic = 0, common = 0;
            uint64_t concatIdentity = 0, concatRange = 0, uses = 0;
            uint64_t sharedStates = 0, sharedReads = 0;
            bool changed() const
            {
                return assigns || algebraic || common || concatIdentity || concatRange ||
                       sharedStates || sharedReads;
            }
        };

        bool shareEquivalentStates(GrhSimModel &model, const SimplifyScope &scope,
                                   CanonicalizeStats &stats)
        {
            std::vector<uint32_t> references(model.states().size() + 1), allowed(references.size()), writers(references.size());
            std::vector<std::string> initial(references.size());
            // Phase scope: a state referenced by any out-of-scope op is part of
            // the partition interface and must not coalesce.
            std::vector<uint8_t> local(references.size(), 1);
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
                if (!scope.inScope(op.phase))
                    for (auto ref : refs)
                        if (ref.kind == ObjectKind::State) local[ref.index] = 0;
                if (name == "core.state.read") ++allowed[refs[0].index];
                else if (name == "core.state.regWrite" || name == "core.state.latchWrite")
                { ++allowed[refs[0].index]; ++writers[refs[0].index]; }
            }
            std::vector<StateId> canonical(references.size());
            std::vector<uint8_t> sharedTargets(references.size());
            for (const auto &state : model.states()) canonical[state.id.index] = state.id;
            std::vector<uint8_t> removeOps(model.operations().size() + 1), removeStates(references.size());
            std::unordered_map<std::string, StateId> groups;
            std::size_t shared = 0;
            for (const auto &op : model.operations())
            {
                if (!scope.inScope(op.phase)) continue;
                const auto name = model.text(op.opType);
                if (name != "core.state.regWrite" && name != "core.state.latchWrite") continue;
                const auto refs = model.objectRefs(op);
                const StateId state{refs[0].index, 0};
                const auto &type = model.types()[model.states()[state.index - 1].type.index - 1];
                if (type.kind != TypeKind::Logic || type.domain != LogicDomain::TwoState ||
                    writers[state.index] != 1 || references[state.index] != allowed[state.index] ||
                    initial[state.index].empty() || !local[state.index]) continue;
                // The writer's phase belongs to the key: stores written in
                // different phases (e.g. a General regWrite vs an Output
                // __tslot_prev write-back) live in different named stores and
                // must not coalesce.
                std::string key = std::to_string(static_cast<unsigned>(op.phase)) + ':' +
                    std::to_string(op.opType.index) + ':' + initial[state.index] + ':';
                for (auto operand : model.operands(op)) key += std::to_string(operand.index) + ',';
                key += ';';
                if (!parameterKey(model.parameters(op), key)) continue;
                const auto [entry, inserted] = groups.emplace(std::move(key), state);
                if (inserted) continue;
                canonical[state.index] = entry->second;
                sharedTargets[entry->second.index] = 1;
                removeStates[state.index] = removeOps[op.id.index] = 1;
                ++shared;
            }
            if (!shared) return false;
            std::vector<ValueId> values(model.values().size() + 1);
            for (const auto &value : model.values()) values[value.id.index] = value.id;
            // Read sharing likewise stays inside one phase: the Event/Output
            // cone clones of a read must not merge with the original.
            std::map<std::tuple<uint32_t, uint32_t, unsigned>, ValueId> reads;
            std::size_t sharedReads = 0;
            for (const auto &op : model.operations())
            {
                if (!scope.inScope(op.phase)) continue;
                if (model.text(op.opType) != "core.state.read" || model.results(op).size() != 1 ||
                    !model.operands(op).empty() || !model.parameters(op).empty()) continue;
                const auto result = model.results(op)[0];
                const auto state = canonical[model.objectRefs(op)[0].index];
                if (!sharedTargets[state.index]) continue;
                const auto type = model.values()[result.index - 1].type;
                const std::tuple key{state.index, type.index, static_cast<unsigned>(op.phase)};
                const auto [entry, inserted] = reads.emplace(key, result);
                if (inserted) continue;
                values[result.index] = entry->second;
                removeOps[op.id.index] = 1;
                ++sharedReads;
            }
            for (const auto &op : model.operations())
            {
                if (removeOps[op.id.index]) continue;
                if (!scope.inScope(op.phase)) continue;
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
            // Provenance: merged states redirect to the surviving canonical
            // state (Direct -> Merged); removed shared reads alias the
            // surviving read value (Direct -> Alias).
            for (std::size_t index = 0; index < canonical.size(); ++index)
                if (!removeStates[index]) canonical[index] = StateId::invalid();
            redirectProvenanceStateSlices(model, canonical, DeclProvenanceKind::Merged);
            // values[i] still maps to itself unless the value was a removed
            // shared read; reduce the map to exactly those redirects.
            for (std::size_t index = 0; index < values.size(); ++index)
                if (values[index] == ValueId{static_cast<uint32_t>(index), 0})
                    values[index] = ValueId::invalid();
            redirectProvenanceValueSlices(model, values, DeclProvenanceKind::Alias);
            model.compact(removeOps, removeStates);
            stats.sharedStates += shared;
            stats.sharedReads += sharedReads;
            return true;
        }
    } // namespace

    SimplifyStepReport simplifyStepCanonicalizeCompute(GrhSimModel &model,
                                                       diag::Diagnostics &diagnostics,
                                                       SimplifyScope scope)
    {
        (void)diagnostics;
        CanonicalizeStats stats;
        {
            auto canonicalizeOnce = [&] {
                std::vector<ValueId> sources(model.values().size() + 1), canonical(sources.size());
                std::vector<uint32_t> producer(sources.size());
                for (const auto &op : model.operations())
                    for (auto value : model.results(op)) producer[value.index] = op.id.index;
                const auto phaseOf = [&](ValueId value) {
                    const auto opIndex = producer[value.index];
                    return opIndex ? model.operations()[opIndex - 1].phase : SimPhase::None;
                };
                // The M1+ cones are sealed by phase: an Event/Output consumer may
                // only read same-phase values, and General-side consumers never
                // read cone values. Every substitution below must keep that
                // barrier or the verifier rejects the rewritten model.
                const auto phaseCompatible = [](SimPhase consumer, SimPhase source) {
                    if (consumer == SimPhase::Event) return source == SimPhase::Event;
                    if (consumer == SimPhase::Output) return source == SimPhase::Output;
                    return source != SimPhase::Event && source != SimPhase::Output;
                };
                for (const auto &op : model.operations())
                {
                    if (!scope.inScope(op.phase)) continue;
                    const auto operands = model.operands(op), results = model.results(op);
                    if (model.text(op.opType) != "core.compute.assign" || operands.size() != 1 || results.size() != 1 ||
                        !model.objectRefs(op).empty() || !model.parameters(op).empty()) continue;
                    const auto typeId = model.values()[results[0].index - 1].type;
                    const auto &type = model.types()[typeId.index - 1];
                    if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                        typeId == model.values()[operands[0].index - 1].type &&
                        phaseCompatible(op.phase, phaseOf(operands[0])))
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
                std::vector<uint32_t> pending(removed.size());
                std::vector<std::vector<uint32_t>> users(removed.size());
                const auto root = [&](ValueId value) {
                    auto current = value;
                    while (canonical[current.index] != current) current = canonical[current.index];
                    while (canonical[value.index] != current)
                    {
                        const auto next = canonical[value.index]; canonical[value.index] = current; value = next;
                    }
                    return current;
                };
                // A concat of consecutive slices of one source is itself a slice of
                // that source; a full-width in-order concat is the source. Packing
                // per-bit registers into words creates this pattern when the old
                // per-bit reads meet an original gather of those bits. Identity
                // folds rewire uses to the source; range folds rewrite the concat
                // in place into one sliceStatic so ids, results and the producer
                // map stay valid for the topological pass below.
                std::size_t concatIdentity = 0, concatRange = 0;
                for (std::size_t index = 0; index < model.operations().size(); ++index)
                {
                    const auto &op = model.operations()[index];
                    if (removed[op.id.index] || !scope.inScope(op.phase) ||
                        model.text(op.opType) != "core.compute.concat") continue;
                    const auto args = model.operands(op), results = model.results(op);
                    if (args.size() < 2 || results.size() != 1 || !model.objectRefs(op).empty() ||
                        !model.parameters(op).empty()) continue;
                    const auto resultTypeId = model.values()[results[0].index - 1].type;
                    const auto &resultType = model.types()[resultTypeId.index - 1];
                    if (resultType.kind != TypeKind::Logic || resultType.domain != LogicDomain::TwoState) continue;
                    ValueId source{}; int64_t low = 0, high = -1; bool match = true;
                    for (std::size_t i = 0; i < args.size() && match; ++i)
                    {
                        const auto value = root(args[i]);
                        const auto pid = producer[value.index];
                        if (!pid || removed[pid]) { match = false; break; }
                        const auto &producerOp = model.operations()[pid - 1];
                        if (model.text(producerOp.opType) != "core.compute.sliceStatic") { match = false; break; }
                        const auto sliceArgs = model.operands(producerOp);
                        const auto sliceParams = model.parameters(producerOp);
                        if (sliceArgs.size() != 1 || !model.objectRefs(producerOp).empty() || sliceParams.size() != 2)
                        { match = false; break; }
                        std::optional<int64_t> start, end;
                        for (const auto &parameter : sliceParams)
                        {
                            const auto *integer = std::get_if<int64_t>(&parameter.value);
                            if (!integer) { match = false; break; }
                            if (model.text(parameter.name) == "sliceStart") start = *integer;
                            else if (model.text(parameter.name) == "sliceEnd") end = *integer;
                            else { match = false; break; }
                        }
                        if (!match || !start || !end || *start < 0 || *end < *start) { match = false; break; }
                        const auto &valueType = model.types()[model.values()[value.index - 1].type.index - 1];
                        if (valueType.kind != TypeKind::Logic || valueType.domain != LogicDomain::TwoState ||
                            valueType.width != static_cast<uint64_t>(*end - *start + 1)) { match = false; break; }
                        const auto sliceSource = root(sliceArgs[0]);
                        if (i == 0) { source = sliceSource; high = *end; }
                        else if (sliceSource != source || low != *end + 1) { match = false; break; }
                        low = *start;
                    }
                    if (!match) continue;
                    const auto &sourceType = model.types()[model.values()[source.index - 1].type.index - 1];
                    if (sourceType.kind != TypeKind::Logic || sourceType.domain != LogicDomain::TwoState ||
                        high >= static_cast<int64_t>(sourceType.width) ||
                        high - low + 1 != static_cast<int64_t>(resultType.width)) continue;
                    if (low == 0 && high + 1 == static_cast<int64_t>(sourceType.width) &&
                        model.values()[source.index - 1].type == resultTypeId &&
                        phaseCompatible(op.phase, phaseOf(source)))
                    {
                        canonical[results[0].index] = source;
                        removed[op.id.index] = 1;
                        ++concatIdentity;
                        continue;
                    }
                    if (resultType.isSigned || !phaseCompatible(op.phase, phaseOf(source))) continue;
                    const std::vector<ValueId> foldOperands{source};
                    const std::vector<ValueId> foldResults(results.begin(), results.end());
                    const std::array foldParams{Parameter{model.intern("sliceStart"), low},
                                                Parameter{model.intern("sliceEnd"), high}};
                    model.replaceOperation(op.id, "core.compute.sliceStatic", foldOperands, foldResults, {}, foldParams);
                    ++concatRange;
                }
                std::vector<uint32_t> ready;
                for (const auto &op : model.operations())
                {
                    if (removed[op.id.index]) continue;
                    for (auto operand : model.operands(op))
                    {
                        users[producer[root(operand).index]].push_back(op.id.index);
                        ++pending[op.id.index];
                    }
                    if (!pending[op.id.index]) ready.push_back(op.id.index);
                }
                std::vector<std::optional<uint64_t>> constants(sources.size());
                std::unordered_map<std::string, ValueId> expressions;
                std::size_t common = 0, algebraic = 0;
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
                        std::vector<ValueId> operands;
                        for (auto operand : model.operands(op)) operands.push_back(root(operand));
                        if (pure)
                        {
                            // Constants stay readable from any scope; only
                            // in-scope ops may be rewritten or removed.
                            constants[results[0].index] = scalarConstant(model, op);
                            if (scope.inScope(op.phase))
                            {
                                if (const auto replacement = simplify(model, op, operands, constants);
                                    replacement && phaseCompatible(op.phase, phaseOf(replacement)))
                                {
                                    canonical[results[0].index] = replacement;
                                    removed[op.id.index] = 1;
                                    ++algebraic;
                                    pure = false;
                                }
                            }
                        }
                        if (pure && scope.inScope(op.phase))
                        {
                            if (operands.size() == 2 && commutative(model.text(op.opType)) &&
                                model.values()[operands[0].index - 1].type == model.values()[operands[1].index - 1].type &&
                                operands[1].index < operands[0].index)
                                std::swap(operands[0], operands[1]);
                            // The phase seals the CSE domain: structurally identical
                            // cone clones (e.g. an Output-phase constant and its
                            // General-side twin) must not merge across the barrier.
                            std::string key = std::to_string(op.opType.index) + ":" +
                                std::to_string(static_cast<unsigned>(op.phase)) + ":" + std::to_string(type.index) + ":";
                            for (auto operand : operands) key += std::to_string(operand.index) + ',';
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
                    }
                    for (auto user : users[op.id.index])
                        if (--pending[user] == 0) ready.push_back(user);
                }
                for (const auto &value : model.values()) canonical[value.id.index] = root(value.id);
                const auto assigns = count;
                count += common + algebraic + concatIdentity + concatRange;
                stats.assigns += assigns;
                stats.algebraic += algebraic;
                stats.common += common;
                stats.concatIdentity += concatIdentity;
                stats.concatRange += concatRange;
                if (count)
                {
                    // Provenance: values folded away here have an equivalent
                    // survivor; redirect their slices (Direct -> Alias) before
                    // compact() drops the removed entities.
                    std::vector<ValueId> redirect(model.values().size() + 1);
                    for (const auto &value : model.values())
                        if (canonical[value.id.index] != value.id)
                            redirect[value.id.index] = canonical[value.id.index];
                    redirectProvenanceValueSlices(model, redirect, DeclProvenanceKind::Alias);
                    for (const auto &op : model.operations())
                    {
                        if (removed[op.id.index]) continue;
                        if (!scope.inScope(op.phase)) continue;
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
                    stats.uses += uses;
                    model.compact(removed, std::vector<uint8_t>(model.states().size() + 1));
                }
            };
            // Same iteration contract as the standalone pass has always had:
            // canonicalize re-enters only after an equivalent-state merge.
            while (true)
            {
                canonicalizeOnce();
                if (!shareEquivalentStates(model, scope, stats)) break;
            }
        }
        SimplifyStepReport report;
        report.changed = stats.changed();
        report.counters = {
            {"identity_assigns_removed", stats.assigns},
            {"algebraic_identities_removed", stats.algebraic},
            {"common_expressions_removed", stats.common},
            {"concat_identity_folds", stats.concatIdentity},
            {"concat_range_folds", stats.concatRange},
            {"rewritten_uses", stats.uses},
            {"equivalent_states_removed", stats.sharedStates},
            {"state_reads_shared", stats.sharedReads},
        };
        return report;
    }

    namespace
    {
        class CanonicalizeComputePass final : public Pass
        {
        public:
            CanonicalizeComputePass() : Pass("grhsim.canonicalize-compute", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto report = simplifyStepCanonicalizeCompute(model, diagnostics,
                                                                    SimplifyScope::whole());
                std::string message;
                for (const auto &[key, count] : report.counters)
                {
                    if (!message.empty()) message += ' ';
                    message += std::string(key) + '=' + std::to_string(count);
                }
                diagnostics.info(std::move(message), name());
                return {true, report.changed, {}};
            }
        };
    } // namespace

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
