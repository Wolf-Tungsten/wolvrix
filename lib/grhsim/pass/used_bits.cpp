#include "grhsim/pass/used_bits.hpp"
#include "grhsim/ir/model.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        // Backward prefix used-bits: bit i of a value is used when some sink can
        // observe it. used == 0 marks a dead cone; 0 < used < width marks
        // unobservable high bits that a narrower value preserves exactly.
        enum class UseRule : uint8_t
        {
            none, sinkFull, inputRead, stateRead, stateWrite,
            assign, transparent, fullUse, shl, mux, bitSelect,
            prioritySelect, concat, replicate, sliceStatic
        };

        UseRule ruleFor(std::string_view name)
        {
            if (name == "core.output.write" || name == "core.system.task" ||
                name == "core.dpi.call" || name == "core.state.memWrite" ||
                name == "core.state.memFill" || name == "core.state.memWriteSeq" ||
                name == "core.state.memAssign" || name == "core.state.memRead" ||
                name == "core.system.function") return UseRule::sinkFull;
            if (name == "core.input.read") return UseRule::inputRead;
            if (name == "core.state.read") return UseRule::stateRead;
            if (name == "core.state.regWrite" || name == "core.state.latchWrite") return UseRule::stateWrite;
            if (!name.starts_with("core.compute.")) return UseRule::none;
            const auto kind = name.substr(std::string_view("core.compute.").size());
            if (kind == "constant") return UseRule::none;
            if (kind == "assign") return UseRule::assign;
            if (kind == "and" || kind == "or" || kind == "xor" || kind == "xnor" ||
                kind == "not" || kind == "add" || kind == "sub" || kind == "mul")
                return UseRule::transparent;
            if (kind == "shl") return UseRule::shl;
            if (kind == "mux") return UseRule::mux;
            if (kind == "bitSelect") return UseRule::bitSelect;
            if (kind == "prioritySelect") return UseRule::prioritySelect;
            if (kind == "concat") return UseRule::concat;
            if (kind == "replicate") return UseRule::replicate;
            if (kind == "sliceStatic") return UseRule::sliceStatic;
            return UseRule::fullUse; // div/mod/right shifts/compares/reductions/logic*/dynamic slices
        }

        bool rebuildable(std::string_view name)
        {
            if (!name.starts_with("core.compute.")) return false;
            const auto kind = name.substr(std::string_view("core.compute.").size());
            return kind == "constant" || kind == "assign" || kind == "and" || kind == "or" ||
                   kind == "xor" || kind == "xnor" || kind == "not" || kind == "add" ||
                   kind == "sub" || kind == "mul" || kind == "shl" || kind == "mux" ||
                   kind == "bitSelect" || kind == "prioritySelect" || kind == "concat" ||
                   kind == "replicate" || kind == "sliceStatic";
        }

        struct UsedBitsAnalysis
        {
            std::vector<uint32_t> used;
            std::vector<uint32_t> usedState;
            std::vector<uint32_t> widthValue;
            std::vector<uint32_t> widthState;
            std::vector<uint32_t> producer; // value index -> op index + 1
            std::vector<uint8_t> rules;
            // Structural narrowing eligibility, independent of the fixpoint:
            // logic two-state, const/random init only, referenced only as
            // read/regWrite/latchWrite target, write data/mask as wide as state.
            std::vector<uint8_t> stateEligible;
            std::vector<std::vector<uint32_t>> opsByState;

            void run(const GrhSimModel &model)
            {
                const auto &types = model.types();
                const auto &ops = model.operations();
                used.assign(model.values().size() + 1, 0);
                usedState.assign(model.states().size() + 1, 0);
                widthValue.assign(model.values().size() + 1, 0);
                widthState.assign(model.states().size() + 1, 0);
                producer.assign(model.values().size() + 1, 0);
                rules.assign(ops.size() + 1, 0);
                opsByState.assign(model.states().size() + 1, {});
                for (const auto &value : model.values())
                {
                    const auto &type = types[value.type.index - 1];
                    if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState)
                        widthValue[value.id.index] = type.width;
                }
                for (const auto &state : model.states())
                {
                    const auto &type = types[state.type.index - 1];
                    if (type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState)
                        widthState[state.id.index] = type.width;
                }
                std::deque<uint32_t> queue;
                std::vector<uint8_t> queued(ops.size() + 1, 0);
                for (const auto &op : ops)
                {
                    rules[op.id.index] = static_cast<uint8_t>(ruleFor(model.text(op.opType)));
                    for (auto result : model.results(op)) producer[result.index] = op.id.index;
                    for (auto ref : model.objectRefs(op))
                        if (ref.kind == ObjectKind::State) opsByState[ref.index].push_back(op.id.index);
                    queue.push_back(op.id.index);
                    queued[op.id.index] = 1;
                }
                stateEligible.assign(model.states().size() + 1, 0);
                for (const auto &state : model.states())
                {
                    if (!widthState[state.id.index]) continue;
                    bool eligible = true;
                    for (const auto &record : model.initRecords())
                        if (record.state == state.id)
                            for (const auto &step : model.steps(record))
                            {
                                const auto kind = model.text(step.kind);
                                if (kind != "core.init.const" && kind != "core.init.random")
                                    eligible = false;
                            }
                    if (eligible)
                        for (const uint32_t opIndex : opsByState[state.id.index])
                        {
                            const auto &op = ops[opIndex - 1];
                            const auto refs = model.objectRefs(op);
                            if (refs.empty() || refs[0].index != state.id.index)
                            {
                                eligible = false; // referenced as an event history
                                break;
                            }
                            const auto rule = static_cast<UseRule>(rules[opIndex]);
                            if (rule == UseRule::stateRead) continue;
                            if (rule == UseRule::stateWrite)
                            {
                                const auto operands = model.operands(op);
                                if (operands.size() < 3 ||
                                    widthValue[operands[1].index] != widthState[state.id.index] ||
                                    widthValue[operands[2].index] != widthState[state.id.index])
                                    eligible = false;
                                continue;
                            }
                            eligible = false;
                        }
                    if (eligible) stateEligible[state.id.index] = 1;
                }
                auto raiseValue = [&](ValueId value, uint32_t request) {
                    if (!value.valid()) return;
                    const uint32_t cap = widthValue[value.index];
                    if (cap == 0) return;
                    request = std::min(request, cap);
                    if (request <= used[value.index]) return;
                    used[value.index] = request;
                    const uint32_t op = producer[value.index];
                    if (op && !queued[op]) { queued[op] = 1; queue.push_back(op); }
                };
                auto raiseState = [&](uint32_t state, uint32_t request) {
                    const uint32_t cap = widthState[state];
                    if (cap == 0) return;
                    request = std::min(request, cap);
                    if (request <= usedState[state]) return;
                    usedState[state] = request;
                    for (const uint32_t op : opsByState[state])
                    {
                        const auto rule = static_cast<UseRule>(rules[op]);
                        if (rule != UseRule::stateWrite) continue;
                        if (!queued[op]) { queued[op] = 1; queue.push_back(op); }
                    }
                };
                while (!queue.empty())
                {
                    const uint32_t opIndex = queue.front();
                    queue.pop_front();
                    queued[opIndex] = 0;
                    const auto &op = ops[opIndex - 1];
                    const auto operands = model.operands(op);
                    const auto results = model.results(op);
                    const auto refs = model.objectRefs(op);
                    const auto rule = static_cast<UseRule>(rules[opIndex]);
                    switch (rule)
                    {
                    case UseRule::none:
                        break;
                    case UseRule::sinkFull:
                        for (auto value : operands) raiseValue(value, widthValue[value.index]);
                        for (auto result : results) raiseValue(result, widthValue[result.index]);
                        break;
                    case UseRule::inputRead:
                        for (auto result : results) raiseValue(result, widthValue[result.index]);
                        break;
                    case UseRule::stateRead:
                        if (!refs.empty() && refs[0].kind == ObjectKind::State && !results.empty())
                            raiseState(refs[0].index, used[results[0].index]);
                        break;
                    case UseRule::stateWrite:
                    {
                        if (refs.empty() || refs[0].kind != ObjectKind::State) break;
                        // Ineligible states never narrow, so their write data/mask
                        // stay fully used; only narrowable states propagate the
                        // state's own used prefix.
                        const uint32_t request = stateEligible[refs[0].index]
                            ? usedState[refs[0].index]
                            : widthValue[operands.size() > 1 ? operands[1].index : 0];
                        if (operands.size() >= 3)
                        {
                            raiseValue(operands[0], widthValue[operands[0].index]);
                            raiseValue(operands[1], request);
                            raiseValue(operands[2], request);
                            for (auto value : operands.subspan(3))
                                raiseValue(value, widthValue[value.index]);
                        }
                        break;
                    }
                    default:
                    {
                        if (results.empty()) break;
                        const uint32_t k = used[results[0].index];
                        if (k == 0) break;
                        switch (rule)
                        {
                        case UseRule::assign:
                            raiseValue(operands[0], k);
                            break;
                        case UseRule::transparent:
                        case UseRule::bitSelect:
                            for (auto value : operands) raiseValue(value, k);
                            break;
                        case UseRule::fullUse:
                            for (auto value : operands) raiseValue(value, widthValue[value.index]);
                            break;
                        case UseRule::shl:
                            raiseValue(operands[0], k);
                            raiseValue(operands[1], widthValue[operands[1].index]);
                            break;
                        case UseRule::mux:
                            raiseValue(operands[0], widthValue[operands[0].index]);
                            raiseValue(operands[1], k);
                            raiseValue(operands[2], k);
                            break;
                        case UseRule::prioritySelect:
                        {
                            const std::size_t count = (operands.size() - 1) / 2;
                            for (std::size_t i = 0; i < count; ++i)
                                raiseValue(operands[i], widthValue[operands[i].index]);
                            for (std::size_t i = count; i < operands.size(); ++i)
                                raiseValue(operands[i], k);
                            break;
                        }
                        case UseRule::concat:
                        {
                            uint32_t offset = 0;
                            for (auto it = operands.rbegin(); it != operands.rend(); ++it)
                            {
                                const uint32_t width = widthValue[it->index];
                                const uint32_t request = k > offset ? std::min(width, k - offset) : 0;
                                raiseValue(*it, request);
                                offset += width;
                            }
                            break;
                        }
                        case UseRule::replicate:
                            raiseValue(operands[0], std::min(k, widthValue[operands[0].index]));
                            break;
                        case UseRule::sliceStatic:
                        {
                            int64_t start = 0;
                            for (const auto &parameter : model.parameters(op))
                                if (model.text(parameter.name) == "sliceStart")
                                    start = std::get<int64_t>(parameter.value);
                            const uint32_t width = widthValue[operands[0].index];
                            const uint64_t request = static_cast<uint64_t>(start) + k;
                            raiseValue(operands[0],
                                       static_cast<uint32_t>(std::min<uint64_t>(width, request)));
                            break;
                        }
                        default:
                            break;
                        }
                        break;
                    }
                    }
                }
            }
        };

        struct UsedBitsStats
        {
            uint64_t deadOps = 0, deadStates = 0;
            uint64_t narrowedValues = 0, downgradedValues = 0;
            uint64_t rebuiltOps = 0, boundarySlices = 0, adaptSlices = 0;
            uint64_t narrowedStates = 0, rebuiltReads = 0, rebuiltWrites = 0;
            uint64_t wideWordsBefore = 0, wideWordsAfter = 0;
        };

        const Parameter *findParameter(const GrhSimModel &model, std::span<const Parameter> parameters,
                                       std::string_view name)
        {
            for (const auto &parameter : parameters)
                if (model.text(parameter.name) == name) return &parameter;
            return nullptr;
        }

        class UsedBitsPass final : public Pass
        {
        public:
            explicit UsedBitsPass(bool analysisOnly)
                : Pass(analysisOnly ? "grhsim.used-bits-analyze" : "grhsim.used-bits",
                       analysisOnly ? PassKind::Analysis : PassKind::SemanticTransform),
                  analysisOnly_(analysisOnly) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                UsedBitsAnalysis analysis;
                analysis.run(model);
                if (analysisOnly_)
                {
                    uint64_t dead = 0, narrowable = 0, downgrades = 0, wideBefore = 0, wideAfter = 0;
                    uint64_t statesDead = 0, statesNarrowed = 0, stateDowngrades = 0;
                    for (const auto &value : model.values())
                    {
                        const uint32_t width = analysis.widthValue[value.id.index];
                        if (!width) continue;
                        const uint32_t used = analysis.used[value.id.index];
                        if (width > 64)
                        {
                            wideBefore += (width + 63) / 64;
                            if (used > 64) wideAfter += (used + 63) / 64;
                        }
                        if (used == 0) { ++dead; continue; }
                        if (used < width)
                        {
                            ++narrowable;
                            if (width > 64 && used <= 64) ++downgrades;
                        }
                    }
                    for (const auto &state : model.states())
                    {
                        const uint32_t width = analysis.widthState[state.id.index];
                        if (!width) continue;
                        const uint32_t used = analysis.usedState[state.id.index];
                        if (used == 0) { ++statesDead; continue; }
                        if (used < width)
                        {
                            ++statesNarrowed;
                            if (width > 64 && used <= 64) ++stateDowngrades;
                        }
                    }
                    diagnostics.info("dead_values=" + std::to_string(dead) +
                                     " narrowable_values=" + std::to_string(narrowable) +
                                     " downgrades=" + std::to_string(downgrades) +
                                     " wide_words_before=" + std::to_string(wideBefore) +
                                     " wide_words_after=" + std::to_string(wideAfter) +
                                     " dead_states=" + std::to_string(statesDead) +
                                     " narrowable_states=" + std::to_string(statesNarrowed) +
                                     " state_downgrades=" + std::to_string(stateDowngrades), name());
                    return {true, false, {}};
                }
                const bool changed = transform(model, analysis, diagnostics);
                return {true, changed, {}};
            }

        private:
            bool analysisOnly_;

            bool transform(GrhSimModel &model, const UsedBitsAnalysis &analysis,
                           diag::Diagnostics &diagnostics)
            {
                const auto &types = model.types();
                const auto valueWidth = [&](ValueId value) -> uint32_t {
                    return types[model.values()[value.index - 1].type.index - 1].width;
                };
                const auto valueSigned = [&](ValueId value) -> bool {
                    return types[model.values()[value.index - 1].type.index - 1].isSigned;
                };
                const std::size_t originalOps = model.operations().size();
                const std::size_t originalStates = model.states().size();
                UsedBitsStats stats;
                for (const auto &value : model.values())
                {
                    const uint32_t width = analysis.widthValue[value.id.index];
                    const uint32_t used = analysis.used[value.id.index];
                    if (width > 64) stats.wideWordsBefore += (width + 63) / 64;
                    if (width > 64 && used > 64) stats.wideWordsAfter += (used + 63) / 64;
                    if (width && used && used < width)
                    {
                        ++stats.narrowedValues;
                        if (width > 64 && used <= 64) ++stats.downgradedValues;
                    }
                }

                // State narrowing decisions reuse the eligibility computed by the
                // analysis (which already forced ineligible states' write operands
                // to full use, so no surviving op can reference a narrowed value
                // through a write that kept its width).
                std::vector<uint32_t> stateNarrow(originalStates + 1, 0);
                for (const auto &state : model.states())
                {
                    const uint32_t width = analysis.widthState[state.id.index];
                    const uint32_t used = analysis.usedState[state.id.index];
                    if (!width || used == 0 || used == width) continue;
                    if (!analysis.stateEligible[state.id.index]) continue;
                    stateNarrow[state.id.index] = used;
                }

                enum class Action : uint8_t { none, rebuild, boundary, handled };
                std::vector<uint8_t> actions(originalOps + 1, 0);
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    const auto &op = model.operations()[i];
                    const auto results = model.results(op);
                    if (results.size() != 1) continue;
                    const ValueId result = results[0];
                    const uint32_t width = analysis.widthValue[result.index];
                    const uint32_t used = analysis.used[result.index];
                    if (!width || used == 0 || used == width) continue;
                    const auto name = model.text(op.opType);
                    if (name == "core.state.read")
                    {
                        const auto refs = model.objectRefs(op);
                        if (!refs.empty() && refs[0].kind == ObjectKind::State && stateNarrow[refs[0].index])
                            actions[op.id.index] = static_cast<uint8_t>(Action::handled);
                        else
                            actions[op.id.index] = static_cast<uint8_t>(Action::boundary);
                        continue;
                    }
                    actions[op.id.index] = rebuildable(name)
                        ? static_cast<uint8_t>(Action::rebuild)
                        : static_cast<uint8_t>(Action::boundary);
                }

                std::vector<ValueId> rewire(model.values().size() + 1);
                std::vector<ValueId> replacement(model.values().size() + 1);
                std::vector<uint8_t> removeOps(originalOps + 1, 0);
                std::vector<uint8_t> removeStates(originalStates + 1, 0);
                std::vector<uint8_t> skipRewire(originalOps + 1, 0);

                // Replacement values for rebuilt ops.
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    const auto &op = model.operations()[i];
                    if (actions[op.id.index] != static_cast<uint8_t>(Action::rebuild)) continue;
                    const ValueId result = model.results(op)[0];
                    const uint32_t used = analysis.used[result.index];
                    const auto narrowType = model.logicType(used, valueSigned(result), LogicDomain::TwoState);
                    const auto narrowed = model.addValue(narrowType, {}, model.values()[result.index - 1].origin);
                    replacement[result.index] = narrowed;
                    rewire[result.index] = narrowed;
                }

                // Boundary slices for non-rebuildable narrowings; reuse existing
                // [0, k) slices so the pass stays idempotent.
                std::unordered_map<uint64_t, std::pair<ValueId, uint32_t>> existingSlices;
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    const auto &op = model.operations()[i];
                    if (model.text(op.opType) != "core.compute.sliceStatic") continue;
                    const auto operands = model.operands(op);
                    const auto results = model.results(op);
                    if (operands.size() != 1 || results.size() != 1) continue;
                    const auto *start = findParameter(model, model.parameters(op), "sliceStart");
                    if (!start || std::get<int64_t>(start->value) != 0) continue;
                    if (actions[op.id.index] != static_cast<uint8_t>(Action::none)) continue;
                    // A reused slice must survive: dead (used == 0) slices are
                    // removed by the dead sweep and cannot serve as replacements.
                    if (analysis.used[results[0].index] != valueWidth(results[0])) continue;
                    existingSlices.emplace((uint64_t(operands[0].index) << 32) | valueWidth(results[0]),
                                           std::make_pair(results[0], op.id.index));
                }
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    const auto &op = model.operations()[i];
                    if (actions[op.id.index] != static_cast<uint8_t>(Action::boundary)) continue;
                    const ValueId result = model.results(op)[0];
                    const uint32_t used = analysis.used[result.index];
                    const uint64_t key = (uint64_t(result.index) << 32) | used;
                    if (const auto found = existingSlices.find(key); found != existingSlices.end())
                    {
                        rewire[result.index] = found->second.first;
                        skipRewire[found->second.second] = 1;
                        continue;
                    }
                    const auto sliceType = model.logicType(used, false, LogicDomain::TwoState);
                    const auto sliced = model.addValue(sliceType, {}, model.values()[result.index - 1].origin);
                    const std::array params{Parameter{model.intern("sliceStart"), int64_t(0)},
                                            Parameter{model.intern("sliceEnd"), int64_t(used - 1)}};
                    model.addOperation("core.compute.sliceStatic", std::array{result}, std::array{sliced},
                                       {}, params, {}, model.values()[result.index - 1].origin);
                    rewire[result.index] = sliced;
                    ++stats.boundarySlices;
                }
                const auto finalOf = [&](ValueId value) {
                    const ValueId mapped = rewire[value.index];
                    return mapped.valid() ? mapped : value;
                };

                // Adaptor: produce a value of exactly target width/sign from a final
                // value, inserting a slice (or an assign for same-width sign
                // conversion). Invalid return signals a prefix-property violation.
                const auto adapt = [&](ValueId value, uint32_t targetWidth, bool targetSigned,
                                       bool scalarCastOk) -> ValueId {
                    const auto &type = types[model.values()[value.index - 1].type.index - 1];
                    if (type.width == targetWidth && type.isSigned == targetSigned) return value;
                    if (type.width == targetWidth && scalarCastOk) return value;
                    if (type.width == targetWidth)
                    {
                        const auto origin = model.values()[value.index - 1].origin;
                        const auto converted = model.addValue(
                            model.logicType(targetWidth, targetSigned, LogicDomain::TwoState), {}, origin);
                        model.addOperation("core.compute.assign", std::array{value}, std::array{converted},
                                           {}, {}, {}, origin);
                        ++stats.adaptSlices;
                        return converted;
                    }
                    if (type.width < targetWidth) return {};
                    if (scalarCastOk && type.width <= 64 && targetWidth <= 64) return value;
                    const auto origin = model.values()[value.index - 1].origin;
                    const auto sliceType = model.logicType(targetWidth, targetSigned, LogicDomain::TwoState);
                    const auto sliced = model.addValue(sliceType, {}, origin);
                    const std::array params{Parameter{model.intern("sliceStart"), int64_t(0)},
                                            Parameter{model.intern("sliceEnd"), int64_t(targetWidth - 1)}};
                    model.addOperation("core.compute.sliceStatic", std::array{value}, std::array{sliced},
                                       {}, params, {}, origin);
                    ++stats.adaptSlices;
                    return sliced;
                };

                // State narrowing, phase 1: create the narrow state, its init and
                // all rebuilt reads. The rewire map must cover every rebuilt read
                // before any write below consults finalOf() — a write to state A
                // can consume a read of state B narrowed later in the same sweep.
                std::vector<StateId> newStateOf(originalStates + 1);
                std::vector<uint32_t> newWidthOf(originalStates + 1, 0);
                std::vector<uint8_t> newSignOf(originalStates + 1, 0);
                for (std::size_t s = 1; s <= originalStates; ++s)
                {
                    const uint32_t narrowed = stateNarrow[s];
                    if (!narrowed) continue;
                    const auto state = model.states()[s - 1];
                    const bool stateSigned = types[state.type.index - 1].isSigned;
                    const auto newType = model.logicType(narrowed, stateSigned, LogicDomain::TwoState);
                    const auto newState = model.addState(model.text(state.name), newType, state.origin);
                    newStateOf[s] = newState;
                    newWidthOf[s] = narrowed;
                    newSignOf[s] = stateSigned;
                    for (std::size_t record = 0; record < model.initRecords().size(); ++record)
                    {
                        const auto initRecord = model.initRecords()[record]; // copy: addInit appends
                        if (initRecord.state != state.id) continue;
                        std::vector<InitStep> steps;
                        std::vector<Parameter> parameters;
                        for (const auto &step : model.steps(initRecord))
                        {
                            const auto stepParams = model.parameters(step);
                            steps.push_back(InitStep{step.kind,
                                                     {static_cast<uint32_t>(parameters.size()),
                                                      static_cast<uint32_t>(stepParams.size())}});
                            parameters.insert(parameters.end(), stepParams.begin(), stepParams.end());
                        }
                        model.addInit(newState, steps, parameters);
                        break; // exactly one init record per state
                    }
                    for (const uint32_t opIndex : analysis.opsByState[s])
                    {
                        const auto op = model.operations()[opIndex - 1];
                        const std::vector<ObjectRef> refs(model.objectRefs(op).begin(), model.objectRefs(op).end());
                        if (refs.empty() || refs[0].index != s) continue; // unreachable: eligibility passed
                        if (model.text(op.opType) != "core.state.read") continue;
                        const ValueId result = model.results(op)[0];
                        if (analysis.used[result.index] == 0) continue; // dead sweep removes it
                        const auto read = model.addValue(newType, {}, model.values()[result.index - 1].origin);
                        model.addOperation("core.state.read", {}, std::array{read},
                                           std::array{ObjectRef::state(newState)}, {},
                                           model.text(op.name), op.origin);
                        rewire[result.index] = read;
                        removeOps[opIndex] = 1;
                        ++stats.rebuiltReads;
                    }
                    removeStates[s] = 1;
                    ++stats.narrowedStates;
                }

                // State narrowing, phase 2: rebuild the write ports.
                for (std::size_t s = 1; s <= originalStates; ++s)
                {
                    if (!newStateOf[s].valid()) continue;
                    const uint32_t narrowed = newWidthOf[s];
                    const bool stateSigned = newSignOf[s] != 0;
                    const StateId newState = newStateOf[s];
                    for (const uint32_t opIndex : analysis.opsByState[s])
                    {
                        const auto op = model.operations()[opIndex - 1];
                        const std::vector<ObjectRef> refs(model.objectRefs(op).begin(), model.objectRefs(op).end());
                        if (refs.empty() || refs[0].index != s) continue;
                        if (model.text(op.opType) == "core.state.read") continue; // phase 1
                        const std::vector<ValueId> args(model.operands(op).begin(), model.operands(op).end());
                        const ValueId data = adapt(finalOf(args[1]), narrowed, stateSigned, false);
                        const ValueId mask = adapt(finalOf(args[2]), narrowed, stateSigned, false);
                        if (!data.valid() || !mask.valid())
                            throw std::runtime_error("used-bits prefix property violated at a state write");
                        std::vector<ValueId> operands{finalOf(args[0]), data, mask};
                        for (std::size_t a = 3; a < args.size(); ++a) operands.push_back(finalOf(args[a]));
                        std::vector<ObjectRef> newRefs{ObjectRef::state(newState)};
                        for (std::size_t r = 1; r < refs.size(); ++r) newRefs.push_back(refs[r]);
                        const std::vector<Parameter> params(model.parameters(op).begin(), model.parameters(op).end());
                        model.addOperation(model.text(op.opType), operands, {}, newRefs, params,
                                           model.text(op.name), op.origin);
                        removeOps[opIndex] = 1;
                        ++stats.rebuiltWrites;
                    }
                }

                // Rebuilt compute ops.
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    if (actions[i + 1] != static_cast<uint8_t>(Action::rebuild)) continue;
                    const auto op = model.operations()[i];
                    const auto name = model.text(op.opType);
                    const auto kind = name.substr(std::string_view("core.compute.").size());
                    // Copy the operand span: adapt() may append to the pools.
                    const std::vector<ValueId> operandCopy(model.operands(op).begin(), model.operands(op).end());
                    const auto operands = std::span<const ValueId>(operandCopy);
                    const ValueId result = model.results(op)[0];
                    const uint32_t used = analysis.used[result.index];
                    const bool sign = valueSigned(result);
                    const ValueId narrowedResult = replacement[result.index];
                    bool ok = true;
                    const auto adaptOrFail = [&](ValueId value, bool castOk) -> ValueId {
                        const ValueId adapted = adapt(finalOf(value), used, sign, castOk);
                        if (!adapted.valid()) ok = false;
                        return adapted;
                    };
                    if (kind == "constant")
                    {
                        const std::vector<Parameter> params(model.parameters(op).begin(), model.parameters(op).end());
                        model.addOperation("core.compute.constant", {}, std::array{narrowedResult}, {}, params,
                                           model.text(op.name), op.origin);
                    }
                    else if (kind == "assign" || kind == "not")
                    {
                        const ValueId operand = adaptOrFail(operands[0], true);
                        if (ok) model.addOperation(model.text(op.opType), std::array{operand},
                                                   std::array{narrowedResult}, {}, {}, model.text(op.name), op.origin);
                    }
                    else if (kind == "and" || kind == "or" || kind == "xor" || kind == "xnor" ||
                             kind == "add" || kind == "sub" || kind == "mul")
                    {
                        const ValueId lhs = adaptOrFail(operands[0], true);
                        const ValueId rhs = adaptOrFail(operands[1], true);
                        if (ok) model.addOperation(model.text(op.opType), std::array{lhs, rhs},
                                                   std::array{narrowedResult}, {}, {}, model.text(op.name), op.origin);
                    }
                    else if (kind == "shl")
                    {
                        const ValueId data = adaptOrFail(operands[0], true);
                        if (ok) model.addOperation("core.compute.shl", std::array{data, finalOf(operands[1])},
                                                   std::array{narrowedResult}, {}, {}, model.text(op.name), op.origin);
                    }
                    else if (kind == "mux")
                    {
                        const ValueId onTrue = adaptOrFail(operands[1], true);
                        const ValueId onFalse = adaptOrFail(operands[2], true);
                        if (ok) model.addOperation("core.compute.mux",
                                                   std::array{finalOf(operands[0]), onTrue, onFalse},
                                                   std::array{narrowedResult}, {}, {}, model.text(op.name), op.origin);
                    }
                    else if (kind == "bitSelect")
                    {
                        const ValueId select = adaptOrFail(operands[0], false);
                        const ValueId onOne = adaptOrFail(operands[1], false);
                        const ValueId onZero = adaptOrFail(operands[2], false);
                        if (ok) model.addOperation("core.compute.bitSelect", std::array{select, onOne, onZero},
                                                   std::array{narrowedResult}, {}, {}, model.text(op.name), op.origin);
                    }
                    else if (kind == "prioritySelect")
                    {
                        std::vector<ValueId> args;
                        const std::size_t count = (operands.size() - 1) / 2;
                        for (std::size_t a = 0; a < count; ++a) args.push_back(finalOf(operands[a]));
                        for (std::size_t a = count; a < operands.size() && ok; ++a)
                        {
                            const ValueId arm = adaptOrFail(operands[a], true);
                            if (ok) args.push_back(arm);
                        }
                        if (ok) model.addOperation("core.compute.prioritySelect", args,
                                                   std::array{narrowedResult}, {}, {}, model.text(op.name), op.origin);
                    }
                    else if (kind == "concat")
                    {
                        std::vector<ValueId> msbFirst;
                        uint32_t offset = 0;
                        for (std::size_t a = operands.size(); a-- > 0 && ok;)
                        {
                            const uint32_t width = valueWidth(operands[a]); // original layout offset
                            const uint32_t need = used > offset ? std::min(width, used - offset) : 0;
                            if (need)
                            {
                                ValueId part = finalOf(operands[a]);
                                const uint32_t partWidth = valueWidth(part);
                                if (partWidth > need)
                                {
                                    const auto sliceType = model.logicType(need, false, LogicDomain::TwoState);
                                    const auto sliced = model.addValue(sliceType, {}, op.origin);
                                    const std::array sliceParams{Parameter{model.intern("sliceStart"), int64_t(0)},
                                                                 Parameter{model.intern("sliceEnd"), int64_t(need - 1)}};
                                    model.addOperation("core.compute.sliceStatic", std::array{part},
                                                       std::array{sliced}, {}, sliceParams, {}, op.origin);
                                    ++stats.adaptSlices;
                                    part = sliced;
                                }
                                else if (partWidth < need) { ok = false; break; }
                                msbFirst.insert(msbFirst.begin(), part);
                            }
                            offset += width;
                        }
                        if (ok && msbFirst.empty()) ok = false;
                        if (ok)
                        {
                            if (msbFirst.size() == 1 && valueWidth(msbFirst[0]) == used)
                                model.addOperation("core.compute.assign", std::array{msbFirst[0]},
                                                   std::array{narrowedResult}, {}, {}, model.text(op.name), op.origin);
                            else if (msbFirst.size() == 1)
                            {
                                const std::array sliceParams{Parameter{model.intern("sliceStart"), int64_t(0)},
                                                             Parameter{model.intern("sliceEnd"), int64_t(used - 1)}};
                                model.addOperation("core.compute.sliceStatic", std::array{msbFirst[0]},
                                                   std::array{narrowedResult}, {}, sliceParams,
                                                   model.text(op.name), op.origin);
                            }
                            else
                                model.addOperation("core.compute.concat", msbFirst, std::array{narrowedResult},
                                                   {}, {}, model.text(op.name), op.origin);
                        }
                    }
                    else if (kind == "replicate")
                    {
                        const uint32_t sourceWidth = valueWidth(operands[0]);
                        if (used <= sourceWidth)
                        {
                            const std::array sliceParams{Parameter{model.intern("sliceStart"), int64_t(0)},
                                                         Parameter{model.intern("sliceEnd"), int64_t(used - 1)}};
                            model.addOperation("core.compute.sliceStatic", std::array{finalOf(operands[0])},
                                               std::array{narrowedResult}, {}, sliceParams,
                                               model.text(op.name), op.origin);
                        }
                        else if (used % sourceWidth == 0)
                        {
                            const std::array repParams{Parameter{model.intern("rep"),
                                                                 int64_t(used / sourceWidth)}};
                            model.addOperation("core.compute.replicate", std::array{finalOf(operands[0])},
                                               std::array{narrowedResult}, {}, repParams,
                                               model.text(op.name), op.origin);
                        }
                        else ok = false;
                    }
                    else if (kind == "sliceStatic")
                    {
                        const auto *start = findParameter(model, model.parameters(op), "sliceStart");
                        const int64_t startBit = start ? std::get<int64_t>(start->value) : 0;
                        const std::array sliceParams{Parameter{model.intern("sliceStart"), startBit},
                                                     Parameter{model.intern("sliceEnd"),
                                                               startBit + int64_t(used) - 1}};
                        model.addOperation("core.compute.sliceStatic", std::array{finalOf(operands[0])},
                                           std::array{narrowedResult}, {}, sliceParams,
                                           model.text(op.name), op.origin);
                    }
                    else ok = false;
                    if (ok)
                    {
                        removeOps[op.id.index] = 1;
                        ++stats.rebuiltOps;
                    }
                    else
                    {
                        // Prefix-property guard: keep the original op, narrow through
                        // a boundary slice on the original full-width result.
                        const std::array params{Parameter{model.intern("sliceStart"), int64_t(0)},
                                                Parameter{model.intern("sliceEnd"), int64_t(used - 1)}};
                        model.addOperation("core.compute.sliceStatic", std::array{result},
                                           std::array{replacement[result.index]}, {}, params, {}, op.origin);
                        ++stats.boundarySlices;
                    }
                }

                // Surviving original ops may still reference narrowed values through
                // consumers that did not narrow themselves (their width fits inside
                // the narrowed operand).
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    if (removeOps[i + 1] || skipRewire[i + 1]) continue;
                    const auto op = model.operations()[i];
                    // Copy every span before adapt() appends to the pools.
                    const std::vector<ValueId> results(model.results(op).begin(), model.results(op).end());
                    if (results.empty()) continue;
                    std::vector<ValueId> updated(model.operands(op).begin(), model.operands(op).end());
                    const std::vector<ObjectRef> outRefs(model.objectRefs(op).begin(), model.objectRefs(op).end());
                    const std::vector<Parameter> outParams(model.parameters(op).begin(), model.parameters(op).end());
                    bool changed = false;
                    const bool isSlice = model.text(op.opType) == "core.compute.sliceStatic";
                    const uint32_t resultWidth = valueWidth(results[0]);
                    for (auto &operand : updated)
                    {
                        const ValueId mapped = rewire[operand.index];
                        if (!mapped.valid()) continue;
                        if (isSlice || valueWidth(mapped) == resultWidth)
                        {
                            operand = mapped;
                            changed = true;
                            continue;
                        }
                        const ValueId adapted = adapt(mapped, resultWidth, valueSigned(results[0]), true);
                        if (!adapted.valid()) continue;
                        operand = adapted;
                        changed = true;
                    }
                    if (!changed) continue;
                    model.replaceOperation(op.id, model.text(op.opType), updated, results, outRefs, outParams);
                }

                // Dead cone: side-effect-free ops whose results are all unused, and
                // writes to states no live reader observes.
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    if (removeOps[i + 1]) continue;
                    const auto &op = model.operations()[i];
                    const auto name = model.text(op.opType);
                    const auto results = model.results(op);
                    // Only two-state logic results participate in used-bits;
                    // string/real/array results (e.g. $display literals) never
                    // have a width and must never count as dead.
                    bool dead = !results.empty();
                    for (auto result : results)
                        dead = dead && analysis.widthValue[result.index] != 0 &&
                               analysis.used[result.index] == 0;
                    if (dead && (name.starts_with("core.compute.") || name == "core.state.read" ||
                                 name == "core.state.memRead"))
                    {
                        removeOps[i + 1] = 1;
                        ++stats.deadOps;
                        continue;
                    }
                    if (name == "core.state.regWrite" || name == "core.state.latchWrite")
                    {
                        const auto refs = model.objectRefs(op);
                        if (!refs.empty() && refs[0].kind == ObjectKind::State &&
                            analysis.widthState[refs[0].index] != 0 &&
                            analysis.usedState[refs[0].index] == 0)
                        {
                            removeOps[i + 1] = 1;
                            ++stats.deadOps;
                        }
                    }
                }
                // States left unreferenced by surviving ops (incl. orphaned event
                // histories) are unobservable; remove logic two-state ones.
                std::vector<uint8_t> referenced(originalStates + 1, 0);
                for (std::size_t i = 0; i < model.operations().size(); ++i)
                {
                    if (i + 1 < removeOps.size() && removeOps[i + 1]) continue;
                    for (auto ref : model.objectRefs(model.operations()[i]))
                        if (ref.kind == ObjectKind::State && ref.index <= originalStates)
                            referenced[ref.index] = 1;
                }
                for (std::size_t s = 1; s <= originalStates; ++s)
                {
                    if (removeStates[s] || referenced[s] || analysis.widthState[s] == 0) continue;
                    removeStates[s] = 1;
                    ++stats.deadStates;
                }

                removeOps.resize(model.operations().size() + 1);
                removeStates.resize(model.states().size() + 1);
                const bool changed = stats.deadOps || stats.deadStates || stats.rebuiltOps ||
                                     stats.boundarySlices || stats.narrowedStates;
                if (changed)
                {
                    // Diagnose dangling references before compact throws opaque.
                    std::vector<uint8_t> removedValues(model.values().size() + 1, 0);
                    for (std::size_t i = 0; i < model.operations().size(); ++i)
                        if (removeOps[i + 1])
                            for (auto result : model.results(model.operations()[i]))
                                removedValues[result.index] = 1;
                    for (std::size_t i = 0; i < model.operations().size(); ++i)
                    {
                        if (removeOps[i + 1]) continue;
                        for (auto value : model.operands(model.operations()[i]))
                            if (removedValues[value.index])
                            {
                                const auto &producer = model.operations()[analysis.producer[value.index] - 1];
                                throw std::runtime_error(
                                    "used-bits dangling value " + std::to_string(value.index) +
                                    " produced by " + std::string(model.text(producer.opType)) +
                                    " consumed by op " + std::to_string(model.operations()[i].id.index) +
                                    " " + std::string(model.text(model.operations()[i].opType)) +
                                    " (originalOps=" + std::to_string(originalOps) + ")");
                            }
                    }
                    model.compact(removeOps, removeStates);
                }
                diagnostics.info("dead_ops_removed=" + std::to_string(stats.deadOps) +
                                 " dead_states_removed=" + std::to_string(stats.deadStates) +
                                 " narrowed_values=" + std::to_string(stats.narrowedValues) +
                                 " downgraded_values=" + std::to_string(stats.downgradedValues) +
                                 " rebuilt_ops=" + std::to_string(stats.rebuiltOps) +
                                 " boundary_slices=" + std::to_string(stats.boundarySlices) +
                                 " adapt_slices=" + std::to_string(stats.adaptSlices) +
                                 " narrowed_states=" + std::to_string(stats.narrowedStates) +
                                 " rebuilt_reads=" + std::to_string(stats.rebuiltReads) +
                                 " rebuilt_writes=" + std::to_string(stats.rebuiltWrites) +
                                 " wide_words_before=" + std::to_string(stats.wideWordsBefore) +
                                 " wide_words_after=" + std::to_string(stats.wideWordsAfter), name());
                return changed;
            }
        };
    }

    void registerUsedBitsPass(PassRegistry &registry)
    {
        std::string error;
        for (bool analysis : {false, true})
            registry.registerPass(analysis ? "grhsim.used-bits-analyze" : "grhsim.used-bits",
                analysis ? PassKind::Analysis : PassKind::SemanticTransform,
                [analysis](std::span<const std::string_view> args, std::string &factoryError)
                    -> std::unique_ptr<Pass> {
                    if (!args.empty())
                    {
                        factoryError = "grhsim.used-bits does not accept arguments";
                        return {};
                    }
                    return std::make_unique<UsedBitsPass>(analysis);
                }, error);
    }
}
