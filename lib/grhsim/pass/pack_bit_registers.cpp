#include "grhsim/pass/pack_bit_registers.hpp"
#include "simplify_internal.hpp"
#include "grhsim/ir/model.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
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

        // Rejection reasons reported as pack_bits_rejected_<reason> counters.
        // The pass runs on the un-partitioned whole graph (stage A5) and
        // decides purely from the raw event_edges annotation plus read/write
        // reference analysis; no CPU schedule or mapping is consulted.
        struct Rejections
        {
            uint64_t targetType = 0;   // not a 1-bit two-state unsigned logic state
            uint64_t init = 0;         // no single known two-state const init
            uint64_t multiWriter = 0;  // writes != 1
            uint64_t extraRefs = 0;    // referenced by anything but plain reads + the one write
            uint64_t noReads = 0;      // never read (dead; left for DCE)
            uint64_t writeShape = 0;   // writer operand/ref layout not packable
            uint64_t writeParams = 0;  // params other than event_edges / bad edge strings
            uint64_t singleton = 0;    // safe candidate whose control signature has no partner
        };

        class PackBitRegistersPass final : public Pass
        {
        public:
            explicit PackBitRegistersPass(std::filesystem::path report = {})
                : Pass("grhsim.pack-bit-registers", PassKind::SemanticTransform), report_(std::move(report)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto originalOps = model.operations().size(), originalStates = model.states().size();
                std::vector<uint32_t> references(originalStates + 1), reads(references.size()), writes(references.size());
                std::vector<std::optional<bool>> initial(references.size());
                Rejections rejected;
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
                // A shared enable/mask, an identical event sequence (same event
                // values, same edges) and the same phase make all lanes update
                // together; their data may be unrelated. The writer must be the
                // only reference besides plain full-width reads, so no other op
                // can observe or update the member state apart from the packed
                // word after the rewrite.
                std::map<std::vector<uint32_t>, std::vector<OpId>> groups;
                for (const auto &op : model.operations())
                {
                    if (model.text(op.opType) != "core.state.regWrite") continue;
                    const auto refs = model.objectRefs(op);
                    const auto operands = model.operands(op);
                    if (refs.empty() || refs[0].kind != ObjectKind::State) continue;
                    const auto target = refs[0].index;
                    if (!bit(model.states()[target - 1].type)) { ++rejected.targetType; continue; }
                    if (!initial[target].has_value()) { ++rejected.init; continue; }
                    if (writes[target] != 1) { ++rejected.multiWriter; continue; }
                    if (references[target] != reads[target] + 1) { ++rejected.extraRefs; continue; }
                    if (!reads[target]) { ++rejected.noReads; continue; }
                    bool valid = model.results(op).empty() && refs.size() == 1 && operands.size() >= 4;
                    for (auto operand : operands) valid &= bit(model.values()[operand.index - 1].type);
                    if (!valid) { ++rejected.writeShape; continue; }
                    const auto parameters = model.parameters(op);
                    if (parameters.size() != 1 || model.text(parameters[0].name) != "event_edges")
                    {
                        ++rejected.writeParams;
                        continue;
                    }
                    const auto *edges = std::get_if<std::vector<std::string>>(&parameters[0].value);
                    if (!edges || edges->empty() || edges->size() + 3 != operands.size())
                    {
                        ++rejected.writeParams;
                        continue;
                    }
                    std::vector<uint32_t> key{operands[0].index, operands[2].index,
                                              static_cast<uint32_t>(op.phase)};
                    bool edgesOk = true;
                    for (std::size_t i = 0; i < edges->size(); ++i)
                    {
                        edgesOk &= (*edges)[i] == "posedge" || (*edges)[i] == "negedge";
                        key.push_back(operands[i + 3].index);
                        key.push_back((*edges)[i] == "posedge");
                    }
                    if (!edgesOk) { ++rejected.writeParams; continue; }
                    groups[std::move(key)].push_back(op.id);
                }
                std::vector<uint8_t> removeOps(originalOps + 1), removeStates(originalStates + 1);
                struct Slice { StateId packed; ValueId read; uint32_t bit = 0; };
                std::vector<Slice> slices(originalStates + 1);
                // Cached before compact() rebuilds dense state IDs; only
                // populated when the report option is enabled.
                struct ReportRow { std::string word; uint32_t bit; std::string member; bool init; };
                std::vector<ReportRow> reportRows;
                uint32_t packedBits = 0, words = 0;
                for (const auto &[key, group] : groups)
                {
                    if (group.size() < 2) { rejected.singleton += group.size(); continue; }
                    for (std::size_t begin = 0; begin < group.size(); begin += 64)
                    {
                        const auto count = std::min<std::size_t>(64, group.size() - begin);
                        if (count < 2) { rejected.singleton += count; continue; }
                        const auto first = model.operations()[group[begin].index - 1];
                        std::vector<ValueId> operands(model.operands(first).begin(), model.operands(first).end());
                        const std::vector<Parameter> params(model.parameters(first).begin(), model.parameters(first).end());
                        const auto type = model.logicType(count, false, LogicDomain::TwoState);
                        const auto name = "packed_bits_" + std::to_string(first.id.index);
                        const auto state = model.addState(name, type, first.origin);
                        const auto read = model.addValue(type), data = model.addValue(type), mask = model.addValue(type);
                        const auto readOp = model.addOperation("core.state.read", {}, std::array{read},
                                                               std::array{ObjectRef::state(state)});
                        model.setOperationPhase(readOp, first.phase);
                        std::vector<ValueId> bits;
                        uint64_t initialWord = 0;
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            const auto op = model.operations()[group[begin + i].index - 1];
                            const auto target = model.objectRefs(op)[0].index;
                            if (!report_.empty())
                                reportRows.push_back({name, static_cast<uint32_t>(i),
                                    std::string(model.text(model.states()[target - 1].name)), *initial[target]});
                            bits.push_back(model.operands(op)[1]);
                            initialWord |= uint64_t(*initial[target]) << i;
                            slices[target] = {state, read, static_cast<uint32_t>(i)};
                            removeStates[target] = 1; removeOps[op.id.index] = 1;
                        }
                        // Lane zero is the low bit; concat inputs run MSB first.
                        std::reverse(bits.begin(), bits.end());
                        const auto concatOp = model.addOperation("core.compute.concat", bits, std::array{data});
                        model.setOperationPhase(concatOp, first.phase);
                        const std::array replicate{Parameter{model.intern("rep"), static_cast<int64_t>(count)}};
                        const auto repOp = model.addOperation("core.compute.replicate", std::array{operands[2]},
                                                              std::array{mask}, {}, replicate);
                        model.setOperationPhase(repOp, first.phase);
                        operands[1] = data; operands[2] = mask;
                        const auto writeOp = model.addOperation("core.state.regWrite", operands, {},
                                                                std::array{ObjectRef::state(state)},
                                                                params, name, first.origin);
                        model.setOperationPhase(writeOp, first.phase);
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
                    const auto slice = slices[refs[0].index];
                    if (!slice.packed.valid()) continue;
                    const std::vector<ValueId> results(model.results(op).begin(), model.results(op).end());
                    const std::array params{Parameter{model.intern("sliceStart"), static_cast<int64_t>(slice.bit)},
                                            Parameter{model.intern("sliceEnd"), static_cast<int64_t>(slice.bit)}};
                    model.replaceOperation(op.id, "core.compute.sliceStatic", std::array{slice.read},
                                           results, {}, params);
                }
                if (words)
                {
                    // Declarations of the packed members re-target to their bit
                    // slice of the packed word (kind=Merged) before compact
                    // drops the old states.
                    std::vector<StateId> mergeTarget(originalStates + 1);
                    std::vector<uint64_t> mergeBase(originalStates + 1);
                    for (std::size_t i = 1; i <= originalStates; ++i)
                    {
                        if (!slices[i].packed.valid()) continue;
                        mergeTarget[i] = slices[i].packed;
                        mergeBase[i] = slices[i].bit;
                    }
                    mergeProvenanceStateSlices(model, mergeTarget, mergeBase, DeclProvenanceKind::Merged);
                    removeOps.resize(model.operations().size() + 1); removeStates.resize(model.states().size() + 1);
                    model.compact(removeOps, removeStates);
                }
                if (!report_.empty())
                {
                    std::ofstream out(report_);
                    if (!out)
                    {
                        diagnostics.error("cannot open pack-bit-registers report", name());
                        return {false, words != 0, {}};
                    }
                    out << "packed_state\tbit_index\tmember_name\tinit_bit\n";
                    for (const auto &row : reportRows)
                        out << row.word << '\t' << row.bit << '\t' << row.member << '\t' << (row.init ? '1' : '0') << '\n';
                    if (!out)
                    {
                        diagnostics.error("cannot write pack-bit-registers report", name());
                        return {false, words != 0, {}};
                    }
                }
                diagnostics.info("packed_register_bits=" + std::to_string(packedBits) +
                                 " packed_register_words=" + std::to_string(words) +
                                 " pack_bits_rejected_target_type=" + std::to_string(rejected.targetType) +
                                 " pack_bits_rejected_init=" + std::to_string(rejected.init) +
                                 " pack_bits_rejected_multi_writer=" + std::to_string(rejected.multiWriter) +
                                 " pack_bits_rejected_extra_refs=" + std::to_string(rejected.extraRefs) +
                                 " pack_bits_rejected_no_reads=" + std::to_string(rejected.noReads) +
                                 " pack_bits_rejected_write_shape=" + std::to_string(rejected.writeShape) +
                                 " pack_bits_rejected_write_params=" + std::to_string(rejected.writeParams) +
                                 " pack_bits_singleton=" + std::to_string(rejected.singleton), name());
                return {true, words != 0, {}};
            }

        private:
            std::filesystem::path report_;
        };
    }

    void registerPackBitRegistersPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass("grhsim.pack-bit-registers", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                std::filesystem::path report;
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    if (i + 1 == args.size()) { factoryError = "grhsim.pack-bit-registers option requires a value"; return {}; }
                    if (args[i] != "--report") { factoryError = "unknown grhsim.pack-bit-registers option"; return {}; }
                    report = args[i + 1];
                }
                return std::make_unique<PackBitRegistersPass>(std::move(report));
            }, error);
    }
}
