#include "grhsim/pass/recover_masked_or.hpp"

#include "grhsim/dialect/compute.hpp"
#include "grhsim/dialect/registry.hpp"
#include "grhsim/ir/model.hpp"
#include "grhsim/pass/pass.hpp"

#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace wolvrix::lib::grhsim
{
    namespace
    {
        const Type &typeOf(const GrhSimModel &model, ValueId value)
        { return model.types()[model.values()[value.index - 1].type.index - 1]; }

        bool twoState(const Type &type)
        { return type.kind == TypeKind::Logic && type.domain == LogicDomain::TwoState && type.width; }

        std::optional<int64_t> integerParameter(const GrhSimModel &model, const SimOp &op, std::string_view name)
        {
            for (const auto &parameter : model.parameters(op))
                if (model.text(parameter.name) == name)
                    if (const auto *value = std::get_if<int64_t>(&parameter.value)) return *value;
            return std::nullopt;
        }

        // Compile-time bit ranges in existing SSA values / constant-index
        // state reads. They never allocate a runtime gathered array.
        struct View
        {
            ValueId value;
            StateId state;
            uint32_t offset = 0;
            friend bool operator==(const View &, const View &) = default;
        };
        struct Term
        {
            View select, data;
            // Remaining factors when a broadcast mask is nested inside ANDs.
            std::vector<ValueId> factors;
        };
        struct Plan
        {
            OpId root;
            ValueId result;
            SimPhase phase;
            uint32_t width;
            std::vector<Term> terms;
        };

        class Matcher
        {
        public:
            const GrhSimModel &model;
            const std::vector<OpId> &producers;

            const SimOp &source(ValueId value) const
            { return model.operations()[producers[value.index].index - 1]; }

            std::optional<slang::SVInt> constantBits(ValueId value) const
            {
                const auto &type = typeOf(model, value);
                const auto &op = source(value);
                if (!twoState(type) || model.text(op.opType) != "core.compute.constant") return std::nullopt;
                for (const auto &param : model.parameters(op))
                {
                    const auto key = model.text(param.name);
                    if (key != "constValue" && key != "value") continue;
                    if (const auto *integer = std::get_if<int64_t>(&param.value))
                        return slang::SVInt(type.width, uint64_t(*integer), type.isSigned);
                    if (const auto *boolean = std::get_if<bool>(&param.value))
                        return slang::SVInt(type.width, uint64_t(*boolean), false);
                    if (const auto *text = std::get_if<std::string>(&param.value))
                    {
                        try
                        {
                            auto bits = slang::SVInt::fromString(*text).resize(type.width);
                            if (!bits.hasUnknown()) return bits;
                        }
                        catch (const std::exception &) {}
                    }
                }
                return std::nullopt;
            }

            bool zero(ValueId value) const
            { const auto bits = constantBits(value); return bits && *bits == 0; }

            std::optional<uint64_t> constant(ValueId value) const
            {
                if (typeOf(model, value).width > 64) return std::nullopt;
                const auto bits = constantBits(value);
                return bits ? std::optional<uint64_t>(bits->getRawPtr()[0]) : std::nullopt;
            }

            bool isOr(const SimOp &op, uint32_t width, SimPhase phase) const
            {
                if (op.phase != phase || model.text(op.opType) != "core.compute.or" ||
                    model.operands(op).size() != 2 || model.results(op).size() != 1 ||
                    !model.objectRefs(op).empty() || !model.parameters(op).empty()) return false;
                const auto sameBits = [&](ValueId v) {
                    const auto &type = typeOf(model, v);
                    return twoState(type) && type.width == width;
                };
                return sameBits(model.results(op)[0]) &&
                       std::all_of(model.operands(op).begin(), model.operands(op).end(), sameBits);
            }

            std::optional<ValueId> alias(ValueId value, SimPhase phase) const
            {
                const auto &op = source(value);
                const auto inputs = model.operands(op);
                if (op.phase != phase || inputs.size() != 1 || !model.objectRefs(op).empty() ||
                    !twoState(typeOf(model, inputs[0])) ||
                    source(inputs[0]).phase != phase ||
                    typeOf(model, inputs[0]).width != typeOf(model, value).width) return std::nullopt;
                const auto kind = model.text(op.opType);
                if ((kind == "core.compute.assign" && model.parameters(op).empty()) ||
                    (kind == "core.compute.replicate" && integerParameter(model, op, "rep") == 1) ||
                    (kind == "core.compute.sliceStatic" && integerParameter(model, op, "sliceStart") == 0 &&
                     integerParameter(model, op, "sliceEnd") == int64_t(typeOf(model, value).width - 1)))
                    return inputs[0];
                return std::nullopt;
            }

            View view(ValueId value, uint32_t offset, uint32_t width, SimPhase phase) const
            {
                // Slices crossing concat / extension boundaries retain the
                // existing SSA value rather than rebuilding it at runtime.
                while (true)
                {
                    const auto &op = source(value);
                    if (op.phase != phase) break;
                    const auto inputs = model.operands(op);
                    const auto kind = model.text(op.opType);
                    if (auto input = alias(value, phase)) { value = *input; continue; }
                    if (kind == "core.compute.assign" && inputs.size() == 1 &&
                        twoState(typeOf(model, inputs[0])) &&
                        source(inputs[0]).phase == phase &&
                        uint64_t(offset) + width <= typeOf(model, inputs[0]).width)
                    { value = inputs[0]; continue; }
                    if (kind == "core.compute.sliceStatic" && inputs.size() == 1 && source(inputs[0]).phase == phase)
                    {
                        const auto lo = integerParameter(model, op, "sliceStart");
                        const auto hi = integerParameter(model, op, "sliceEnd");
                        if (lo && hi && *lo >= 0 && *hi >= *lo &&
                            uint64_t(*hi - *lo + 1) == typeOf(model, value).width &&
                            uint64_t(*lo) + offset + width <= typeOf(model, inputs[0]).width)
                        { offset += uint32_t(*lo); value = inputs[0]; continue; }
                    }
                    if (kind == "core.compute.replicate" && inputs.size() == 1 && source(inputs[0]).phase == phase)
                    {
                        const auto stride = typeOf(model, inputs[0]).width;
                        if (stride && offset % stride + uint64_t(width) <= stride)
                        { offset %= stride; value = inputs[0]; continue; }
                    }
                    if (kind == "core.compute.concat")
                    {
                        uint64_t start = 0;
                        bool found = false;
                        for (auto it = inputs.rbegin(); it != inputs.rend(); ++it)
                        {
                            const auto bits = typeOf(model, *it).width;
                            if (offset >= start && uint64_t(offset) + width <= start + bits && source(*it).phase == phase)
                            { offset -= uint32_t(start); value = *it; found = true; break; }
                            start += bits;
                        }
                        if (found) continue;
                    }
                    const auto refs = model.objectRefs(op);
                    const bool state = kind == "core.state.readArray" && refs.size() == 1 &&
                        refs[0].kind == ObjectKind::State;
                    const bool slice = kind == "core.compute.sliceArray" && inputs.size() >= 2 && refs.empty() &&
                        source(inputs[0]).phase == phase;
                    if ((state || slice) && model.parameters(op).empty())
                    {
                        const auto &base = state ? model.types()[model.states()[refs[0].index - 1].type.index - 1]
                                                 : typeOf(model, inputs[0]);
                        const auto indices = inputs.subspan(state ? 0 : 1);
                        uint64_t stride = base.width, start = 0;
                        bool valid = twoState(base) && !indices.empty() && indices.size() <= base.shape.size();
                        for (std::size_t i = 0; valid && i < indices.size(); ++i)
                        {
                            const auto row = constant(indices[i]);
                            valid = base.shape[i] > 0 && stride % base.shape[i] == 0 && row &&
                                    *row < uint64_t(base.shape[i]);
                            if (!valid) break;
                            stride /= base.shape[i]; start += *row * stride;
                        }
                        valid &= stride == typeOf(model, value).width && start + offset + width <= base.width;
                        if (valid)
                        {
                            offset += uint32_t(start);
                            if (state) return {{}, StateId{refs[0].index, 0}, offset};
                            value = inputs[0]; continue;
                        }
                    }
                    break;
                }
                return {value, {}, offset};
            }

            std::optional<ValueId> broadcast(ValueId value, uint32_t width, SimPhase phase) const
            {
                while (auto input = alias(value, phase)) value = *input;
                if (width == 1) return value;
                if (typeOf(model, value).width < width) return std::nullopt;
                const auto &op = source(value);
                const auto inputs = model.operands(op);
                if (op.phase != phase || inputs.size() != 1 || !model.objectRefs(op).empty() ||
                    typeOf(model, inputs[0]).width != 1 || !twoState(typeOf(model, inputs[0]))) return std::nullopt;
                if ((model.text(op.opType) == "core.compute.replicate" &&
                     integerParameter(model, op, "rep") == int64_t(typeOf(model, value).width) &&
                     typeOf(model, value).width >= width) ||
                    (model.text(op.opType) == "core.compute.assign" && typeOf(model, inputs[0]).isSigned))
                    return inputs[0];
                return std::nullopt;
            }

            std::optional<Term> term(ValueId value, SimPhase phase, uint32_t width) const
            {
                while (auto input = alias(value, phase)) value = *input;
                const auto &op = source(value);
                if (op.phase != phase || !model.objectRefs(op).empty() || !model.parameters(op).empty()) return std::nullopt;
                const auto inputs = model.operands(op);
                const auto kind = model.text(op.opType);
                const auto pair = [&](ValueId select, ValueId data) -> std::optional<Term> {
                    if (!twoState(typeOf(model, select)) || typeOf(model, select).width != 1 ||
                        !twoState(typeOf(model, data)) || typeOf(model, data).width < width ||
                        source(select).phase != phase || source(data).phase != phase) return std::nullopt;
                    return Term{view(select, 0, 1, phase), view(data, 0, width, phase), {}};
                };
                if ((kind == "core.compute.mux" || (kind == "core.compute.bitSelect" && width == 1)) &&
                    inputs.size() == 3 && zero(inputs[2])) return pair(inputs[0], inputs[1]);
                if (kind != "core.compute.and" || inputs.size() != 2) return std::nullopt;
                std::optional<Term> best;
                for (uint32_t side = 0; side < 2; ++side)
                    if (auto select = broadcast(inputs[side], width, phase))
                        if (auto current = pair(*select, inputs[1 - side]))
                        {
                            // Prefer packed SSA selectors to state-array data.
                            const auto score = [&](const Term &t) {
                                if (!t.select.value) return std::pair{false, uint32_t(0)};
                                const auto &type = typeOf(model, t.select.value);
                                return std::pair{type.shape.empty(), type.width};
                            };
                            if (!best || score(*current) > score(*best)) best = current;
                        }
                if (best || width == 1) return best;
                // Pull one broadcast factor through associative ANDs. Keep
                // sibling expressions as SSA values; shared/observed producers
                // survive cleanup. A visited set prevents DAG expansion.
                struct Factor { ValueId value, sibling; uint32_t parent; };
                const auto none = std::numeric_limits<uint32_t>::max();
                std::vector<Factor> factors{{value, {}, none}};
                std::unordered_set<uint32_t> visited;
                for (std::size_t i = 0; i < factors.size(); ++i)
                {
                    const auto current = factors[i].value;
                    if (!visited.insert(current.index).second) continue;
                    if (auto select = broadcast(current, width, phase))
                    {
                        if (!twoState(typeOf(model, *select)) || typeOf(model, *select).width != 1 ||
                            source(*select).phase != phase) continue;
                        Term result{view(*select, 0, 1, phase), {}, {}};
                        for (auto node = uint32_t(i); factors[node].parent != none; node = factors[node].parent)
                            result.factors.push_back(factors[node].sibling);
                        if (!result.factors.empty()) return result;
                    }
                    const auto stripped = [&] {
                        auto v = current;
                        while (auto input = alias(v, phase)) v = *input;
                        return v;
                    }();
                    const auto &node = source(stripped);
                    const auto args = model.operands(node);
                    if (node.phase != phase || model.text(node.opType) != "core.compute.and" ||
                        !model.parameters(node).empty() || !model.objectRefs(node).empty() || args.size() != 2 ||
                        !twoState(typeOf(model, args[0])) || !twoState(typeOf(model, args[1])) ||
                        typeOf(model, args[0]).width < width || typeOf(model, args[1]).width < width ||
                        source(args[0]).phase != phase || source(args[1]).phase != phase) continue;
                    factors.push_back({args[0], args[1], uint32_t(i)});
                    factors.push_back({args[1], args[0], uint32_t(i)});
                }
                return std::nullopt;
            }
        };

        class RecoverMaskedOrPass final : public Pass
        {
        public:
            explicit RecoverMaskedOrPass(uint32_t minTerms, std::filesystem::path report)
                : Pass("cpu.st.recover-masked-or", PassKind::SemanticTransform),
                  minTerms_(minTerms), report_(std::move(report)) {}

            PassResult run(GrhSimModel &model, diag::Diagnostics &diagnostics) override
            {
                const auto initialOps = model.operations().size();
                std::vector<OpId> producers(model.values().size() + 1), consumer(producers.size());
                std::vector<uint32_t> uses(producers.size(), 0);
                std::vector<uint8_t> observed(producers.size(), 0);
                for (const auto &op : model.operations())
                {
                    if (op.phase == SimPhase::None)
                    {
                        diagnostics.error("recover-masked-or requires total phase attribution; run "
                                          "grhsim.split-phases first", name());
                        return {false, false, {}};
                    }
                    for (const auto result : model.results(op)) producers[result.index] = op.id;
                    for (const auto operand : model.operands(op))
                    { ++uses[operand.index]; consumer[operand.index] = op.id; }
                }
                for (const auto &decl : model.declProvenances())
                    for (const auto &slice : decl.slices)
                        if (slice.target == DeclProvenanceTarget::Value && slice.targetIndex < observed.size())
                            observed[slice.targetIndex] = 1;
                const Matcher matcher{model, producers};
                std::ofstream report, coverage;
                if (!report_.empty())
                {
                    if (!report_.parent_path().empty()) std::filesystem::create_directories(report_.parent_path());
                    report.open(report_);
                    coverage.open(report_.string() + ".candidates.tsv");
                    if (!report || !coverage)
                    { diagnostics.error("cannot open maskedOr reports: " + report_.string(), name()); return {false, false, {}}; }
                    report << "phase\troot_before_compact\tterms\telement_width\tarray_state\tarray_value\tmode\tselect_sources\tdata_sources\n";
                    coverage << "phase\troot_before_compact\tterms\telement_width\treason\trecovered_subregions\n";
                }
                std::vector<Plan> plans;
                uint64_t candidates = 0, longCandidates = 0, full = 0;
                // Each private tree is built once. Bottom-up validity allows
                // useful subreductions inside a non-matching outer OR.
                struct Node
                {
                    ValueId value;
                    std::vector<uint32_t> children;
                    std::optional<Term> term;
                    uint32_t leaves = 1;
                    bool valid = false, isOr = false;
                };
                for (const auto &op : model.operations())
                {
                    if (op.phase != SimPhase::Event && op.phase != SimPhase::General && op.phase != SimPhase::Output)
                        continue;
                    const auto results = model.results(op);
                    if (results.size() != 1) continue;
                    const auto result = results[0];
                    const auto width = typeOf(model, result).width;
                    if (!matcher.isOr(op, width, op.phase)) continue;
                    auto parentValue = result;
                    bool internal = false;
                    while (!observed[parentValue.index] && uses[parentValue.index] == 1)
                    {
                        const auto &parent = model.operations()[consumer[parentValue.index].index - 1];
                        if (matcher.isOr(parent, width, op.phase)) { internal = true; break; }
                        const auto outputs = model.results(parent);
                        if (outputs.size() != 1 || matcher.alias(outputs[0], op.phase) != parentValue) break;
                        parentValue = outputs[0];
                    }
                    if (internal) continue;
                    ++candidates;
                    std::vector<Node> nodes{{result, {}, {}, 1, false, true}};
                    for (std::size_t i = 0; i < nodes.size(); ++i)
                    {
                        const auto value = nodes[i].value;
                        const auto &producer = matcher.source(value);
                        std::vector<ValueId> children;
                        if (i == 0 || (!observed[value.index] && uses[value.index] == 1))
                        {
                            if (matcher.isOr(producer, width, op.phase))
                            {
                                nodes[i].isOr = true;
                                children.assign(model.operands(producer).begin(), model.operands(producer).end());
                            }
                            else if (auto input = matcher.alias(value, op.phase)) children.push_back(*input);
                        }
                        if (children.empty())
                        {
                            nodes[i].term = matcher.term(value, op.phase, width);
                            nodes[i].valid = nodes[i].term.has_value();
                        }
                        for (const auto child : children)
                        {
                            const auto index = uint32_t(nodes.size());
                            nodes[i].children.push_back(index);
                            nodes.push_back({child, {}, {}, 1, false, false});
                        }
                    }
                    for (std::size_t i = nodes.size(); i-- > 0;)
                        if (!nodes[i].children.empty())
                        {
                            nodes[i].leaves = 0; nodes[i].valid = true;
                            for (const auto child : nodes[i].children)
                            { nodes[i].leaves += nodes[child].leaves; nodes[i].valid &= nodes[child].valid; }
                        }
                    const auto before = plans.size();
                    std::vector<uint32_t> stack{0};
                    while (!stack.empty())
                    {
                        const auto index = stack.back(); stack.pop_back();
                        const auto &node = nodes[index];
                        if (node.isOr && node.valid && node.leaves >= minTerms_)
                        {
                            Plan plan{matcher.source(node.value).id, node.value, op.phase, width, {}};
                            std::vector<uint32_t> leaves{index};
                            while (!leaves.empty())
                            {
                                const auto leaf = leaves.back(); leaves.pop_back();
                                if (nodes[leaf].term) plan.terms.push_back(*nodes[leaf].term);
                                else for (const auto child : nodes[leaf].children) leaves.push_back(child);
                            }
                            plans.push_back(std::move(plan));
                        }
                        else for (const auto child : node.children) stack.push_back(child);
                    }
                    if (nodes[0].leaves >= minTerms_)
                    {
                        ++longCandidates; full += nodes[0].valid;
                        if (coverage.is_open())
                            coverage << toString(op.phase) << '\t' << op.id.index << '\t' << nodes[0].leaves << '\t'
                                     << width << '\t' << (nodes[0].valid ? "recovered" : "unmasked_or_boundary_leaf")
                                     << '\t' << plans.size() - before << '\n';
                    }
                }
                if (!plans.empty())
                {
                    const auto *cpu = defaultDialectRegistry().findDialect("cpu");
                    model.addDialect(cpu->name, cpu->version, cpu->schemaFingerprint);
                }
                std::vector<std::pair<ValueId, SimPhase>> dead;
                std::array<uint64_t, 5> byPhase{};
                uint64_t terms = 0, denseRegions = 0;
                for (const auto &plan : plans)
                {
                    const auto origin = model.operations()[plan.root.index - 1].origin;
                    std::vector<ValueId> inputs;
                    std::map<std::pair<uint32_t, uint32_t>, uint32_t> sourceSlots;
                    const auto slot = [&](const View &view) {
                        const auto key = std::pair{view.value.index, view.state.index};
                        if (const auto it = sourceSlots.find(key); it != sourceSlots.end()) return it->second;
                        ValueId value = view.value;
                        if (view.state)
                        {
                            value = model.addValue(model.states()[view.state.index - 1].type, {}, origin);
                            const auto read = model.addOperation("core.state.read", {}, std::array{value},
                                std::array{ObjectRef::state(view.state)}, {}, {}, origin);
                            model.setOperationPhase(read, plan.phase);
                            producers.resize(model.values().size() + 1); producers[value.index] = read;
                            uses.resize(producers.size()); observed.resize(producers.size());
                        }
                        const auto index = uint32_t(inputs.size());
                        sourceSlots.emplace(key, index); inputs.push_back(value);
                        return index;
                    };
                    std::vector<int64_t> views;
                    std::vector<uint32_t> selectors, data;
                    for (const auto &term : plan.terms)
                    {
                        auto dataView = term.data;
                        if (!term.factors.empty())
                        {
                            auto data = term.factors[0];
                            const auto type = model.logicType(plan.width, false, LogicDomain::TwoState);
                            for (std::size_t i = 1; i < term.factors.size(); ++i)
                            {
                                const auto result = model.addValue(type, {}, origin);
                                const auto op = model.addOperation("core.compute.and", std::array{data, term.factors[i]},
                                                                   std::array{result}, {}, {}, {}, origin);
                                model.setOperationPhase(op, plan.phase);
                                producers.resize(model.values().size() + 1); producers[result.index] = op;
                                uses.resize(producers.size()); observed.resize(producers.size());
                                ++uses[data.index]; ++uses[term.factors[i].index]; data = result;
                            }
                            dataView = {data, {}, 0};
                        }
                        const auto selectSlot = slot(term.select), elementSlot = slot(dataView);
                        views.insert(views.end(), {int64_t(selectSlot), int64_t(term.select.offset),
                                                  int64_t(elementSlot), int64_t(dataView.offset)});
                        selectors.push_back(selectSlot); data.push_back(elementSlot);
                    }
                    // Keep the compact two-operand encoding for complete aligned
                    // arrays. All other layouts use static view descriptors.
                    bool dense = inputs.size() == 2;
                    const auto maskSlot = uint32_t(views[0]), dataSlot = uint32_t(views[2]);
                    const auto n = plan.terms.size();
                    const auto &maskType = typeOf(model, inputs[maskSlot]);
                    const auto &arrayType = typeOf(model, inputs[dataSlot]);
                    dense &= maskSlot != dataSlot && maskType.shape.empty() && maskType.width == n &&
                             arrayType.shape.size() == 1 && arrayType.shape[0] == int64_t(n) &&
                             arrayType.width == uint64_t(n) * plan.width;
                    std::vector<uint8_t> selected(n);
                    for (std::size_t i = 0; dense && i < n; ++i)
                    {
                        const auto bit = uint64_t(views[4 * i + 1]);
                        dense = views[4 * i] == maskSlot && views[4 * i + 2] == dataSlot && bit < n &&
                                !selected[bit] && uint64_t(views[4 * i + 3]) == bit * plan.width;
                        if (dense) selected[bit] = 1;
                    }
                    std::vector<Parameter> params;
                    if (dense) { inputs = {inputs[maskSlot], inputs[dataSlot]}; ++denseRegions; }
                    else params.push_back({model.intern("views"), std::move(views)});
                    const auto &root = model.operations()[plan.root.index - 1];
                    for (const auto old : model.operands(root))
                    { --uses[old.index]; dead.emplace_back(old, plan.phase); }
                    for (const auto input : inputs) ++uses[input.index];
                    model.replaceOperation(plan.root, "cpu.compute.maskedOr", inputs,
                                           std::array{plan.result}, {}, params);
                    ++byPhase[static_cast<std::size_t>(plan.phase)]; terms += n;
                    if (report.is_open())
                    {
                        const auto unique = [](auto &values) {
                            std::sort(values.begin(), values.end());
                            return std::unique(values.begin(), values.end()) - values.begin();
                        };
                        const auto first = plan.terms[0].data;
                        report << toString(plan.phase) << '\t' << plan.root.index << '\t' << n << '\t'
                               << plan.width << '\t' << (dense ? first.state.index : 0) << '\t'
                               << (dense ? first.value.index : 0) << '\t' << (dense ? "dense" : "views") << '\t'
                               << unique(selectors) << '\t' << unique(data) << '\n';
                    }
                }
                std::vector<uint8_t> remove(model.operations().size() + 1, 0);
                uint64_t removed = 0, removedReads = 0;
                while (!dead.empty())
                {
                    const auto [value, phase] = dead.back(); dead.pop_back();
                    const auto id = producers[value.index];
                    if (!id || remove[id.index] || observed[value.index] || uses[value.index] != 0) continue;
                    const auto &op = model.operations()[id.index - 1];
                    const auto kind = model.text(op.opType);
                    if (op.phase != phase || !(isPureComputeOp(kind) || kind == "core.state.read" ||
                                              kind == "core.state.readArray" || kind == "core.input.read")) continue;
                    bool unused = true;
                    for (const auto result : model.results(op)) unused &= !observed[result.index] && uses[result.index] == 0;
                    if (!unused) continue;
                    remove[id.index] = 1; ++removed;
                    if (kind == "core.state.readArray") ++removedReads;
                    for (const auto input : model.operands(op))
                    { --uses[input.index]; dead.emplace_back(input, phase); }
                }
                if (!plans.empty()) model.compact(remove, std::vector<uint8_t>(model.states().size() + 1));
                diagnostics.info("masked_or_candidates=" + std::to_string(candidates) +
                    " long_candidates=" + std::to_string(longCandidates) + " full_recoveries=" + std::to_string(full) +
                    " masked_or_regions=" + std::to_string(plans.size()) + " dense_regions=" + std::to_string(denseRegions) +
                    " masked_or_terms=" + std::to_string(terms) +
                    " masked_or_event=" + std::to_string(byPhase[static_cast<std::size_t>(SimPhase::Event)]) +
                    " masked_or_general=" + std::to_string(byPhase[static_cast<std::size_t>(SimPhase::General)]) +
                    " masked_or_output=" + std::to_string(byPhase[static_cast<std::size_t>(SimPhase::Output)]) +
                    " removed_ops=" + std::to_string(removed) + " removed_array_reads=" + std::to_string(removedReads) +
                    " initial_ops=" + std::to_string(initialOps), name());
                if (!report_.empty())
                {
                    report.flush(); coverage.flush();
                    if (!report.good() || !coverage.good())
                    { diagnostics.error("failed writing maskedOr reports", name()); return {false, !plans.empty(), {}}; }
                }
                return {true, !plans.empty(), report_.empty() ? std::vector<std::string>{} :
                    std::vector<std::string>{report_.string(), report_.string() + ".candidates.tsv"}};
            }

        private:
            uint32_t minTerms_;
            std::filesystem::path report_;
        };
    }

    void registerRecoverMaskedOrPass(PassRegistry &registry)
    {
        std::string error;
        registry.registerPass("cpu.st.recover-masked-or", PassKind::SemanticTransform,
            [](std::span<const std::string_view> args, std::string &factoryError) -> std::unique_ptr<Pass> {
                uint32_t minTerms = 16;
                std::filesystem::path report;
                for (std::size_t i = 0; i < args.size(); i += 2)
                {
                    if (i + 1 == args.size())
                    { factoryError = "recover-masked-or options require --key value pairs"; return {}; }
                    if (args[i] == "--report") report = args[i + 1];
                    else if (args[i] == "--min-terms")
                    {
                        const auto value = args[i + 1];
                        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), minTerms);
                        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                            minTerms < 2 || minTerms > std::numeric_limits<uint32_t>::max() / 2)
                        { factoryError = "recover-masked-or --min-terms must be in [2,2147483647]"; return {}; }
                    }
                    else { factoryError = "unknown recover-masked-or option: " + std::string(args[i]); return {}; }
                }
                return std::make_unique<RecoverMaskedOrPass>(minTerms, std::move(report));
            }, error);
    }
}
