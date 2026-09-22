#include "grhsim/pass/pack_bit_registers.hpp"
#include "grhsim/ir/model.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        std::optional<bool> initialBit(const GrhSimModel &model, const InitRecord &record)
        {
            const auto steps = model.steps(record);
            if (steps.size() != 1 || model.text(steps[0].kind) != "core.init.const") return {};
            const auto parameters = model.parameters(steps[0]);
            if (parameters.size() != 1 || model.text(parameters[0].name) != "value") return {};
            const auto *literal = std::get_if<std::string>(&parameters[0].value);
            if (!literal) return {};
            try
            {
                const auto value = slang::SVInt::fromString(*literal).resize(1);
                if (value.hasUnknown()) return {};
                return value.as<uint64_t>().value_or(0) != 0;
            }
            catch (const std::exception &) { return {}; }
        }

        class PackBitRegistersPass final : public Pass
        {
        public:
            PackBitRegistersPass() : Pass("grhsim.pack-bit-registers", PassKind::SemanticTransform) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto *mapping = model.cpuMapping();
                if (!mapping || !mapping->schedule)
                {
                    diagnostics.error("requires a CPU schedule to preserve quiescence projection", name());
                    return {false, false, {}};
                }
                // Mixing projected and private bits would make private changes
                // request extra evaluation rounds. Keep their words separate.
                const auto projection = mapping->schedule->quiescenceProjection;
                const auto originalOps = model.operations().size(), originalStates = model.states().size();
                std::vector<uint32_t> references(originalStates + 1), reads(references.size()), writes(references.size());
                std::vector<std::optional<bool>> initial(references.size());
                const auto bit = [&](TypeId id) {
                    const auto &type = model.types()[id.index - 1];
                    return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState &&
                           type.width == 1 && !type.isSigned;
                };
                for (const auto &record : model.initRecords())
                    if (bit(model.states()[record.state.index - 1].type)) initial[record.state.index] = initialBit(model, record);
                for (const auto &op : model.operations())
                {
                    const auto refs = model.objectRefs(op);
                    for (auto ref : refs) if (ref.kind == ObjectKind::State) ++references[ref.index];
                    const auto type = model.text(op.opType);
                    if (type == "core.state.read")
                    {
                        if (refs.size() == 1 && refs[0].kind == ObjectKind::State &&
                            model.results(op).size() == 1 && model.parameters(op).empty() &&
                            model.operands(op).empty() &&
                            model.values()[model.results(op)[0].index - 1].type == model.states()[refs[0].index - 1].type)
                            ++reads[refs[0].index];
                    }
                    else if (type == "core.state.regWrite" && !refs.empty() && refs[0].kind == ObjectKind::State)
                        ++writes[refs[0].index];
                }
                // A shared enable/mask and identical private event histories
                // make all lanes update together; their data may be unrelated.
                std::map<std::vector<uint32_t>, std::vector<OpId>> groups;
                for (const auto &op : model.operations())
                {
                    if (model.text(op.opType) != "core.state.regWrite") continue;
                    const auto refs = model.objectRefs(op);
                    const auto operands = model.operands(op);
                    if (refs.empty() || refs[0].kind != ObjectKind::State) continue;
                    const auto target = refs[0].index;
                    if (!bit(model.states()[target - 1].type) || !initial[target].has_value() ||
                        writes[target] != 1 || references[target] != reads[target] + 1 || !reads[target] ||
                        operands.size() < 4 || refs.size() != operands.size() - 2) continue;
                    bool valid = model.results(op).empty();
                    for (auto ref : refs) valid &= ref.kind == ObjectKind::State;
                    if (!valid) continue;
                    for (auto operand : operands) valid &= bit(model.values()[operand.index - 1].type);
                    const auto parameters = model.parameters(op);
                    if (parameters.size() != 1 || model.text(parameters[0].name) != "event_edges") continue;
                    const auto *edges = std::get_if<std::vector<std::string>>(&parameters[0].value);
                    if (!edges || edges->empty() || edges->size() + 3 != operands.size()) continue;
                    std::vector<uint32_t> key{operands[0].index, operands[2].index, uint32_t(projection[target])};
                    for (std::size_t i = 0; i < edges->size(); ++i)
                    {
                        const auto history = refs[i + 1].index;
                        valid &= (*edges)[i] == "posedge" || (*edges)[i] == "negedge";
                        valid &= references[history] == 1 && bit(model.states()[history - 1].type) && initial[history].has_value();
                        key.push_back(operands[i + 3].index);
                        key.push_back((*edges)[i] == "posedge");
                        key.push_back(initial[history].value_or(false));
                    }
                    if (valid) groups[std::move(key)].push_back(op.id);
                }
                std::vector<uint8_t> removeOps(originalOps + 1), removeStates(originalStates + 1);
                struct Slice { ValueId packed; uint32_t bit = 0; };
                std::vector<Slice> slices(originalStates + 1);
                uint32_t packedBits = 0, words = 0;
                for (const auto &[key, group] : groups)
                {
                    for (std::size_t begin = 0; begin < group.size(); begin += 64)
                    {
                        const auto count = std::min<std::size_t>(64, group.size() - begin);
                        if (count < 2) continue;
                        const auto first = model.operations()[group[begin].index - 1];
                        std::vector<ValueId> operands(model.operands(first).begin(), model.operands(first).end());
                        std::vector<ObjectRef> refs(model.objectRefs(first).begin(), model.objectRefs(first).end());
                        const std::vector<Parameter> params(model.parameters(first).begin(), model.parameters(first).end());
                        const auto type = model.logicType(count, false, LogicDomain::TwoState);
                        const auto name = "packed_bits_" + std::to_string(first.id.index);
                        const auto state = model.addState(name, type, first.origin);
                        const auto read = model.addValue(type, {}, first.origin), data = model.addValue(type, {}, first.origin), mask = model.addValue(type, {}, first.origin);
                        model.addOperation("core.state.read", {}, std::array{read}, std::array{ObjectRef::state(state)}, {}, {}, first.origin);
                        std::vector<ValueId> bits;
                        uint64_t initialWord = 0;
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            const auto op = model.operations()[group[begin + i].index - 1];
                            const auto target = model.objectRefs(op)[0].index;
                            bits.push_back(model.operands(op)[1]);
                            initialWord |= uint64_t(*initial[target]) << i;
                            slices[target] = {read, static_cast<uint32_t>(i)};
                            removeStates[target] = 1; removeOps[op.id.index] = 1;
                            if (i)
                                for (auto history : model.objectRefs(op).subspan(1)) removeStates[history.index] = 1;
                        }
                        // Lane zero is the low bit; concat inputs run MSB first.
                        std::reverse(bits.begin(), bits.end());
                        model.addOperation("core.compute.concat", bits, std::array{data}, {}, {}, {}, first.origin);
                        const std::array replicate{Parameter{model.intern("rep"), static_cast<int64_t>(count)}};
                        model.addOperation("core.compute.replicate", std::array{operands[2]}, std::array{mask}, {}, replicate, {}, first.origin);
                        operands[1] = data; operands[2] = mask; refs[0] = ObjectRef::state(state);
                        model.addOperation("core.state.regWrite", operands, {}, refs, params, name, first.origin);
                        const std::array initParams{Parameter{model.intern("value"),
                            std::to_string(count) + "'d" + std::to_string(initialWord)}};
                        const std::array steps{InitStep{model.intern("core.init.const"), {0, 1}}};
                        model.addInit(state, steps, initParams);
                        packedBits += count; ++words;
                    }
                }
                // Preserve each old read result's users, including commit
                // snapshots. Mapping reconstructs the packed state's fanout.
                for (std::size_t i = 0; i < originalOps; ++i)
                {
                    const auto op = model.operations()[i];
                    if (model.text(op.opType) != "core.state.read") continue;
                    const auto refs = model.objectRefs(op);
                    if (refs.size() != 1 || refs[0].kind != ObjectKind::State) continue;
                    const auto slice = slices[model.objectRefs(op)[0].index];
                    if (!slice.packed) continue;
                    const std::vector<ValueId> results(model.results(op).begin(), model.results(op).end());
                    const std::array params{Parameter{model.intern("sliceStart"), static_cast<int64_t>(slice.bit)},
                                            Parameter{model.intern("sliceEnd"), static_cast<int64_t>(slice.bit)}};
                    model.replaceOperation(op.id, "core.compute.sliceStatic", std::array{slice.packed}, results, {}, params);
                }
                if (words)
                {
                    removeOps.resize(model.operations().size() + 1); removeStates.resize(model.states().size() + 1);
                    model.compact(removeOps, removeStates);
                }
                diagnostics.info("packed_register_bits=" + std::to_string(packedBits) +
                                 " packed_register_words=" + std::to_string(words), name());
                return {true, words != 0, {}};
            }
        };
    }

    void registerPackBitRegistersPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass("grhsim.pack-bit-registers", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                if (!args.empty()) { factoryError = "grhsim.pack-bit-registers does not accept arguments"; return {}; }
                return std::make_unique<PackBitRegistersPass>();
            }, error);
    }
}
