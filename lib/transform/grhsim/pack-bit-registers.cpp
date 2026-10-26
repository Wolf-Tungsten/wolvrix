#include "transform/grhsim/pack-bit-registers.hpp"

#include "rewrite.hpp"
#include "transform/verify.hpp"

#include <fstream>
#include <map>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
struct Candidate {
    OperationId state, writer;
    std::vector<OperationId> reads;
    bool initial = false;
    ValueId mask;
};
}

PackBitRegistersPass::PackBitRegistersPass(PackBitRegistersOptions options)
    : Pass("grhsim-pack-bit-registers", "grhsim-pack-bit-registers", "Pack scalar bit states with shared fire controls"),
      options_(std::move(options)) {}

bool PackBitRegistersPass::supportsDialect(std::string_view dialect) const noexcept { return detail::supports(dialect); }

PassResult PackBitRegistersPass::run()
{
    PassResult result;
    try {
        auto& graph = detail::selectGraph(design(), options_.top);
        if (graph.metadata(kStage) != std::optional<AttributeValue>(std::string("converted")))
            throw std::invalid_argument("Bit-register packing requires the unpartitioned converted graph");
        PassManager verifier; verifier.addPass(std::make_unique<GrhVerifyPass>());
        if (!verifier.run(design(), diags()).success) return {false, true, {}};
        std::ofstream report;
        if (!options_.report.empty()) {
            if (!options_.report.parent_path().empty()) std::filesystem::create_directories(options_.report.parent_path());
            report.open(options_.report);
            if (!report) throw std::runtime_error("Cannot write packing report: " + options_.report.string());
            report << "packed_state\tbit\tmember\tinitial\n";
        }
        detail::Scope scope(graph, {});
        const LogicalType bit{ValueType::Logic, 1, false, LogicDomain::TwoState, {}};
        std::unordered_map<OperationId, std::vector<OperationId>, OperationIdHash> references;
        for (const auto op : graph.operations())
            if (const auto name = attribute<std::string>(graph, op, "regSymbol"))
                references[graph.findOperation(*name)].push_back(op);
        std::map<std::string, std::vector<Candidate>> groups;
        for (const auto target : graph.operations()) {
            if (graph.opKind(target) != OperationKind::kRegister || stateLogicalType(graph, target) != bit) continue;
            const auto init = stateInitSpec(graph, target);
            if (init.size() != 1 || init[0].kind != InitKind::Constant) continue;
            const auto initial = slang::SVInt::fromString(init[0].value).resize(1);
            if (initial.hasUnknown()) continue;
            Candidate candidate{target, {}, {}, !detail::isZero(initial), {}};
            bool valid = true;
            for (const auto ref : references[target]) {
                const auto kind = graph.opKind(ref);
                if (kind == OperationKind::kRegisterReadPort && graph.opResults(ref).size() == 1 &&
                    graph.valueLogicalType(graph.opResults(ref)[0]) == bit) candidate.reads.push_back(ref);
                else if ((kind == kWrite || kind == kWriteMask) && !candidate.writer && !scope.anchored(ref)) candidate.writer = ref;
                else { valid = false; break; }
            }
            if (!valid || !candidate.writer || candidate.reads.empty()) continue;
            const auto args = graph.opOperands(candidate.writer);
            if (!std::all_of(args.begin(), args.end(), [&](auto value) { return graph.valueLogicalType(value) == bit; })) continue;
            if (graph.opKind(candidate.writer) == kWriteMask) {
                const auto mask = detail::constantBits(graph, args[3]);
                if (!mask || !detail::isOnes(*mask)) candidate.mask = args[3];
            }
            std::ostringstream key;
            for (const auto value : {args[0], args[1], candidate.mask}) key << value.index << ',' << value.generation << ';';
            groups[key.str()].push_back(std::move(candidate));
        }
        std::size_t bits = 0, words = 0;
        for (const auto& [key, group] : groups) {
            for (std::size_t begin = 0; begin < group.size(); begin += 64) {
                const auto count = std::min<std::size_t>(64, group.size() - begin);
                if (count < 2) continue;
                const auto& first = group[begin];
                const std::vector<ValueId> controls(graph.opOperands(first.writer).begin(), graph.opOperands(first.writer).end());
                LogicalType word = bit; word.width = count;
                const auto state = graph.createOperation(OperationKind::kRegister);
                setStateLogicalType(graph, state, word);
                const auto stateName = std::string(graph.getOperation(state).symbolText());
                uint64_t initial = 0;
                std::vector<ValueId> lanes;
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& member = group[begin + i];
                    initial |= static_cast<uint64_t>(member.initial) << i;
                    lanes.push_back(graph.opOperands(member.writer)[2]);
                    detail::mergeOrigins(graph, state, {member.state});
                    if (report.is_open()) report << stateName << '\t' << i << '\t'
                        << graph.getOperation(member.state).symbolText() << '\t' << member.initial << '\n';
                }
                setStateInitSpec(graph, state, {{InitKind::Constant, std::to_string(count) + "'d" + std::to_string(initial)}});
                const auto read = graph.createOperation(OperationKind::kRegisterReadPort);
                const auto current = graph.createValue(word);
                graph.addResult(read, current); graph.setAttr(read, "regSymbol", stateName);
                for (std::size_t i = 0; i < count; ++i)
                    for (const auto source : group[begin + i].reads) detail::mergeOrigins(graph, read, {source});
                std::reverse(lanes.begin(), lanes.end());
                const auto data = detail::makeOperation(graph, first.writer, OperationKind::kConcat, word, lanes);
                const auto writer = graph.createOperation(first.mask ? kWriteMask : kWrite);
                for (const auto value : {controls[0], controls[1], data}) graph.addOperand(writer, value);
                if (first.mask) {
                    const auto mask = detail::makeOperation(graph, first.writer, OperationKind::kReplicate, word, std::array{first.mask});
                    graph.setAttr(graph.valueDef(mask), "rep", static_cast<int64_t>(count)); graph.addOperand(writer, mask);
                }
                graph.setAttr(writer, "regSymbol", stateName);
                graph.setAttr(writer, "writerOrder", attribute<int64_t>(graph, first.writer, "writerOrder").value());
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& member = group[begin + i];
                    detail::mergeOrigins(graph, writer, {member.writer});
                    detail::mergeOrigins(graph, graph.valueDef(data), {member.writer});
                    for (const auto oldRead : member.reads) {
                        detail::rewrite(graph, oldRead, OperationKind::kSliceStatic, std::array{current});
                        graph.setAttr(oldRead, "sliceStart", static_cast<int64_t>(i));
                        graph.setAttr(oldRead, "sliceEnd", static_cast<int64_t>(i));
                    }
                    graph.eraseOp(member.writer);
                    detail::replaceStateWithView(graph, member.state, current, i);
                }
                bits += count; ++words; result.changed = true;
            }
        }
        if (report.is_open()) {
            report.flush(); if (!report) throw std::runtime_error("Failed writing packing report");
            result.artifacts.push_back(options_.report.string());
        }
        logInfo("packed_bits=" + std::to_string(bits) + " words=" + std::to_string(words));
        if (!verifier.run(design(), diags()).success) result.failed = true;
    } catch (const std::exception& error) { this->error(error.what()); result.failed = true; }
    return result;
}

} // namespace wolvrix::lib::transform::grhsim
