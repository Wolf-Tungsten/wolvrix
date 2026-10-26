#include "transform/grhsim/recover-masked-or.hpp"

#include "rewrite.hpp"
#include "transform/verify.hpp"

#include <fstream>
#include <map>

namespace wolvrix::lib::transform::grhsim {
namespace {
using namespace grh;
struct View { ValueId value; OperationId state; int64_t offset = 0; };
struct Term { View select, data; std::vector<ValueId> factors; std::vector<OperationId> origins; };

struct Matcher {
    detail::Scope& scope;
    Graph& graph;
    int32_t width;

    bool twoState(ValueId value) const { return detail::twoState(graph.valueLogicalType(value)); }
    std::optional<slang::SVInt> constant(ValueId value) const {
        return scope.local(value) ? detail::constantBits(graph, value) : std::nullopt;
    }
    bool zero(ValueId value) const { const auto bits = constant(value); return bits && detail::isZero(*bits); }
    bool isOr(OperationId op) const {
        if (!scope.contains(op) || graph.opKind(op) != OperationKind::kOr || graph.opOperands(op).size() != 2 ||
            graph.opResults(op).size() != 1) return false;
        if (!twoState(graph.opResults(op)[0]) || graph.valueWidth(graph.opResults(op)[0]) != width) return false;
        for (const auto value : graph.opOperands(op)) if (!twoState(value) || graph.valueWidth(value) != width) return false;
        return true;
    }
    ValueId strip(ValueId value) const {
        for (unsigned depth = 0; depth < 128; ++depth) {
            const auto op = graph.valueDef(value);
            if (!scope.contains(op)) break;
            const auto args = graph.opOperands(op);
            if (args.size() != 1 || !twoState(args[0]) || graph.valueWidth(args[0]) != graph.valueWidth(value)) break;
            const auto kind = graph.opKind(op);
            if (kind != OperationKind::kAssign && kind != OperationKind::kConcat &&
                !(kind == OperationKind::kSliceStatic && attribute<int64_t>(graph, op, "sliceStart") == 0)) break;
            value = args[0];
        }
        return value;
    }
    View view(ValueId value, int64_t offset, int32_t bits) const {
        for (unsigned depth = 0; depth < 128; ++depth) {
            const auto op = graph.valueDef(value);
            if (!scope.contains(op)) break;
            const auto args = graph.opOperands(op);
            const auto kind = graph.opKind(op);
            const auto alias = strip(value);
            if (alias != value) { value = alias; continue; }
            if (kind == OperationKind::kSliceStatic && args.size() == 1) {
                const auto lo = attribute<int64_t>(graph, op, "sliceStart").value();
                if (lo + offset + bits <= graph.valueWidth(args[0])) { offset += lo; value = args[0]; continue; }
            }
            if (kind == OperationKind::kReplicate && args.size() == 1) {
                const auto stride = graph.valueWidth(args[0]);
                if (offset % stride + bits <= stride) { offset %= stride; value = args[0]; continue; }
            }
            if (kind == OperationKind::kConcat || kind == OperationKind::kArrayCreate) {
                int64_t start = 0; bool found = false;
                for (std::size_t i = 0; i < args.size(); ++i) {
                    const auto part = args[kind == OperationKind::kConcat ? args.size() - i - 1 : i];
                    const auto size = graph.valueWidth(part);
                    if (offset >= start && offset + bits <= start + size) { offset -= start; value = part; found = true; break; }
                    start += size;
                }
                if (found) continue;
            }
            const bool read = kind == OperationKind::kRegisterReadArrayPort;
            if (read || kind == OperationKind::kSliceArray) {
                const auto state = read ? graph.findOperation(attribute<std::string>(graph, op, "regSymbol").value()) : OperationId{};
                const auto base = read ? stateLogicalType(graph, state) : graph.valueLogicalType(args[0]);
                const auto indices = args.subspan(read ? 0 : 1);
                int64_t stride = base.width, start = 0; bool valid = indices.size() <= base.shape.size();
                for (std::size_t i = 0; valid && i < indices.size(); ++i) {
                    const auto index = constant(indices[i]);
                    const auto row = index && !index->hasUnknown() && !index->isNegative() ? index->as<uint64_t>() : std::nullopt;
                    if (!row || *row >= static_cast<uint64_t>(base.shape[i])) { valid = false; break; }
                    stride /= base.shape[i]; start += static_cast<int64_t>(*row) * stride;
                }
                if (valid && start + offset + bits <= base.width) {
                    offset += start;
                    if (read) return {{}, state, offset};
                    value = args[0]; continue;
                }
            }
            break;
        }
        return {value, {}, offset};
    }
    std::optional<ValueId> broadcast(ValueId value) const {
        value = strip(value);
        if (width == 1 && graph.valueWidth(value) == 1) return value;
        const auto op = graph.valueDef(value);
        if (!scope.contains(op) || graph.opKind(op) != OperationKind::kReplicate || graph.opOperands(op).size() != 1) return {};
        const auto bit = graph.opOperands(op)[0];
        return graph.valueWidth(bit) == 1 && graph.valueWidth(value) >= width && twoState(bit) ? std::optional(bit) : std::nullopt;
    }
    std::optional<Term> term(ValueId value) const {
        value = strip(value);
        const auto op = graph.valueDef(value);
        if (!scope.contains(op)) return {};
        const auto args = graph.opOperands(op);
        const auto kind = graph.opKind(op);
        if ((kind == OperationKind::kMux || (kind == kBitSelect && width == 1)) && args.size() == 3 &&
            graph.valueWidth(args[0]) == 1 && twoState(args[0]) && twoState(args[1]) &&
            graph.valueWidth(args[1]) >= width && zero(args[2]))
            return Term{view(args[0], 0, 1), view(args[1], 0, width), {}, {op}};
        if (kind != OperationKind::kAnd || args.size() != 2) return {};
        struct Factor { ValueId value, sibling; std::size_t parent; };
        constexpr auto none = std::numeric_limits<std::size_t>::max();
        std::vector<Factor> factors{{value, {}, none}};
        std::unordered_set<ValueId, ValueIdHash> seen;
        for (std::size_t i = 0; i < factors.size() && i < 4096; ++i) {
            const auto current = strip(factors[i].value);
            if (!seen.insert(current).second) continue;
            if (const auto bit = broadcast(current)) {
                Term term{view(*bit, 0, 1), {}, {}, {op}};
                for (auto node = i; factors[node].parent != none; node = factors[node].parent) {
                    term.factors.push_back(factors[node].sibling);
                    if (const auto origin = graph.valueDef(factors[node].value)) term.origins.push_back(origin);
                }
                if (!term.factors.empty()) {
                    if (term.factors.size() == 1) { term.data = view(term.factors[0], 0, width); term.factors.clear(); }
                    return term;
                }
            }
            const auto node = graph.valueDef(current);
            if (!scope.contains(node) || graph.opKind(node) != OperationKind::kAnd || graph.opOperands(node).size() != 2) continue;
            const auto inputs = graph.opOperands(node);
            if (!twoState(inputs[0]) || !twoState(inputs[1]) || graph.valueWidth(inputs[0]) < width || graph.valueWidth(inputs[1]) < width) continue;
            factors.push_back({inputs[0], inputs[1], i}); factors.push_back({inputs[1], inputs[0], i});
        }
        return {};
    }
};

bool recover(detail::Scope& scope, uint32_t minimum, std::ostream* report, std::size_t& regions)
{
    auto& graph = scope.graph;
    bool changed = false;
    const std::vector<OperationId> operations(graph.operations().begin(), graph.operations().end());
    for (const auto op : operations) {
        if (!scope.contains(op) || graph.opResults(op).size() != 1) continue;
        const auto result = graph.opResults(op)[0];
        Matcher matcher{scope, graph, graph.valueWidth(result)};
        if (!matcher.isOr(op)) continue;
        const auto resultView = graph.getValue(result);
        const auto uses = resultView.users();
        if (!scope.exported(result) && uses.size() == 1 && matcher.isOr(uses[0].operation)) continue;
        struct Node { ValueId value; std::vector<std::size_t> children; std::optional<Term> term; bool valid = false; uint32_t leaves = 1; };
        std::vector<Node> nodes{{result, {}, {}}};
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto value = nodes[i].value;
            const auto source = graph.valueDef(value);
            if ((i == 0 || (graph.valueUsers(value).size() == 1 && !scope.exported(value))) && matcher.isOr(source)) {
                const std::vector<ValueId> inputs(graph.opOperands(source).begin(), graph.opOperands(source).end());
                for (const auto input : inputs) {
                    const auto child = nodes.size(); nodes[i].children.push_back(child); nodes.push_back({input, {}, {}});
                }
            } else { nodes[i].term = matcher.term(value); nodes[i].valid = nodes[i].term.has_value(); }
        }
        for (std::size_t i = nodes.size(); i-- > 0;)
            if (!nodes[i].children.empty()) {
                nodes[i].leaves = 0; nodes[i].valid = true;
                for (const auto child : nodes[i].children) { nodes[i].valid &= nodes[child].valid; nodes[i].leaves += nodes[child].leaves; }
            }
        std::vector<std::size_t> pending{0};
        while (!pending.empty()) {
            const auto root = pending.back(); pending.pop_back();
            if (!nodes[root].valid || nodes[root].leaves < minimum || nodes[root].children.empty()) {
                for (const auto child : nodes[root].children) pending.push_back(child);
                continue;
            }
            const auto target = graph.valueDef(nodes[root].value);
            std::vector<Term> terms; std::vector<std::size_t> stack{root};
            while (!stack.empty()) {
                const auto index = stack.back(); stack.pop_back();
                const auto origin = graph.valueDef(nodes[index].value);
                if (origin) detail::mergeOrigins(graph, target, {origin});
                if (nodes[index].term) terms.push_back(*nodes[index].term);
                else for (const auto child : nodes[index].children) stack.push_back(child);
            }
            std::vector<ValueId> inputs; std::vector<int64_t> views;
            std::map<std::pair<uint32_t, uint32_t>, int64_t> slots;
            const auto slot = [&](const View& view) {
                const auto key = std::pair{view.value.index, view.state.index};
                if (const auto found = slots.find(key); found != slots.end()) return found->second;
                auto value = view.value;
                if (view.state) {
                    value = detail::makeOperation(graph, target, OperationKind::kRegisterReadPort, stateLogicalType(graph, view.state), {});
                    graph.setAttr(graph.valueDef(value), "regSymbol", std::string(graph.getOperation(view.state).symbolText()));
                }
                const auto index = static_cast<int64_t>(inputs.size()); inputs.push_back(value); slots.emplace(key, index); return index;
            };
            for (auto& term : terms) {
                for (const auto origin : term.origins) detail::mergeOrigins(graph, target, {origin});
                if (!term.factors.empty()) {
                    auto data = term.factors[0];
                    const LogicalType type{ValueType::Logic, matcher.width, false, LogicDomain::TwoState, {}};
                    for (std::size_t i = 1; i < term.factors.size(); ++i)
                        data = detail::makeOperation(graph, target, OperationKind::kAnd, type, std::array{data, term.factors[i]});
                    term.data = {data, {}, 0};
                }
                views.insert(views.end(), {slot(term.select), term.select.offset, slot(term.data), term.data.offset});
            }
            bool dense = inputs.size() == 2;
            const auto mask = views[0], data = views[2];
            const auto count = static_cast<int64_t>(terms.size());
            dense &= mask != data && graph.valueShape(inputs[mask]).empty() && graph.valueWidth(inputs[mask]) == count &&
                graph.valueLogicalType(inputs[data]).shape == std::vector<int64_t>{count} &&
                graph.valueWidth(inputs[data]) == count * matcher.width;
            std::set<int64_t> selected;
            for (std::size_t i = 0; dense && i < views.size(); i += 4)
                dense = views[i] == mask && views[i + 2] == data && views[i + 1] < count &&
                    selected.insert(views[i + 1]).second && views[i + 3] == views[i + 1] * matcher.width;
            if (dense) inputs = {inputs[mask], inputs[data]};
            detail::rewrite(graph, target, kMaskedOr, inputs);
            if (!dense) graph.setAttr(target, "views", views);
            if (report) *report << graph.getOperation(target).symbolText() << '\t' << terms.size() << '\t'
                << matcher.width << '\t' << (dense ? "dense" : "views") << '\n';
            ++regions; changed = true;
        }
    }
    return changed;
}
}

RecoverMaskedOrPass::RecoverMaskedOrPass(RecoverMaskedOrOptions options)
    : Pass("grhsim-recover-masked-or", "grhsim-recover-masked-or", "Recover two-state masked reductions"), options_(std::move(options)) {}
bool RecoverMaskedOrPass::supportsDialect(std::string_view dialect) const noexcept { return detail::supports(dialect); }
PassResult RecoverMaskedOrPass::run()
{
    PassResult result;
    try {
        if (options_.minTerms < 2) throw std::invalid_argument("min-terms must be at least 2");
        auto& graph = detail::selectGraph(design(), options_.top);
        PassManager verifier; verifier.addPass(std::make_unique<GrhVerifyPass>());
        if (!verifier.run(design(), diags()).success) return {false, true, {}};
        std::ofstream report;
        if (!options_.report.empty()) {
            if (!options_.report.parent_path().empty()) std::filesystem::create_directories(options_.report.parent_path());
            report.open(options_.report); if (!report) throw std::runtime_error("Cannot open maskedOr report");
            report << "root\tterms\telement_width\tmode\n";
        }
        std::set<detail::Owner> owners;
        for (const auto op : graph.operations()) if (operationEffect(graph, op) != OpEffect::Declaration) owners.insert(detail::owner(graph, op));
        std::size_t regions = 0;
        for (const auto& owner : owners) {
            detail::Scope scope(graph, owner);
            result.changed = recover(scope, options_.minTerms, report.is_open() ? &report : nullptr, regions) || result.changed;
        }
        // Cleanup follows all matching, so each stored candidate ID remains live.
        for (const auto& owner : owners) { detail::Scope scope(graph, owner); result.changed = scope.dce() || result.changed; }
        if (report.is_open()) {
            report.flush(); if (!report) throw std::runtime_error("Failed writing maskedOr report");
            result.artifacts.push_back(options_.report.string());
        }
        logInfo("masked_or_regions=" + std::to_string(regions));
        if (!verifier.run(design(), diags()).success) result.failed = true;
    } catch (const std::exception& error) { this->error(error.what()); result.failed = true; }
    return result;
}

} // namespace wolvrix::lib::transform::grhsim
