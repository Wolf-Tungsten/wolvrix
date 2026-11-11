#include "transform/common_expr.hpp"

#include "core/op_descriptor.hpp"
#include "ingest/node_attributes.hpp"
#include "transform/grhsim/ops.hpp"
#include "transform/verify.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wolvrix::lib::transform {
namespace {

using namespace grh;

constexpr std::size_t kMinimumCommonExpressions = 5;
constexpr int32_t kBasicWidth = 256;

void requireWholeGrh(Graph& graph)
{
    if (graph.metadata(grhsim::kStage))
        throw std::invalid_argument("common-expr requires an unpartitioned GRH graph before grh-to-grhsim");
    for (const auto op : graph.operations())
        if (grhsim::attribute<std::vector<std::string>>(graph, op, grhsim::kPartition))
            throw std::invalid_argument("common-expr requires an unpartitioned GRH graph");
}

void verifyWhole(Graph& graph)
{
    if (const auto problem = grhsim::verifyGraph(graph); !problem.empty())
        throw std::invalid_argument(problem);
    for (const auto op : graph.operations())
        if (const auto problem = verifyOpDescriptor(graph, op); !problem.empty())
            throw std::invalid_argument(problem);
}

bool pureCandidate(const Graph& graph, OperationId op)
{
    if (!op || operationEffect(graph, op) != OpEffect::Pure ||
        graph.opKind(op) == OperationKind::kConstant || graph.opResults(op).size() != 1)
        return false;
    const auto result = graph.opResults(op)[0];
    const auto type = graph.valueLogicalType(result);
    if (type.kind != ValueType::Logic || !type.shape.empty() ||
        graph.valueIsInput(result) || graph.valueIsOutput(result) || graph.valueIsInout(result))
        return false;
    return true;
}

void encodeAttribute(std::ostringstream& out, const AttributeValue& value)
{
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) out << (item ? "b1" : "b0");
        else if constexpr (std::is_same_v<T, int64_t>) out << "i" << item;
        else if constexpr (std::is_same_v<T, double>) out << "d" << std::bit_cast<uint64_t>(item);
        else if constexpr (std::is_same_v<T, std::string>) out << "s" << item.size() << ":" << item;
        else if constexpr (std::is_same_v<T, std::vector<bool>>) {
            out << "vb" << item.size() << ":";
            for (const bool bit : item) out << (bit ? '1' : '0');
        } else if constexpr (std::is_same_v<T, std::vector<int64_t>>) {
            out << "vi" << item.size() << ":";
            for (const auto value : item) out << value << ",";
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
            out << "vd" << item.size() << ":";
            for (const auto value : item) out << std::bit_cast<uint64_t>(value) << ",";
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            out << "vs" << item.size() << ":";
            for (const auto& value : item) out << value.size() << ":" << value;
        }
    }, value);
}

using ExpressionId = std::size_t;

std::string opKey(const Graph& graph, OperationId op, std::span<const ExpressionId> operands)
{
    std::ostringstream out;
    const auto results = graph.opResults(op);
    if (results.size() != 1) return {};
    const auto result = results[0];
    const auto type = graph.valueLogicalType(result);
    out << "O" << static_cast<unsigned>(graph.opKind(op)) << ":"
        << static_cast<unsigned>(type.kind) << ":" << type.width << ":"
        << (type.isSigned ? 1 : 0) << ":" << static_cast<unsigned>(type.domain) << ":";
    out << type.shape.size() << ":";
    for (const auto dim : type.shape) out << dim << ",";
    std::vector<std::pair<std::string, std::string>> attrs;
    for (const auto attr : graph.opAttrRange(op)) {
        if (attr.key() == kOriginModulesKey || attr.key() == "grhsim.nodeClone" ||
            ingest::node::isAttribute(attr.key())) continue;
        std::ostringstream value;
        encodeAttribute(value, attr.value().toOwned());
        attrs.emplace_back(std::string(attr.key()), value.str());
    }
    std::sort(attrs.begin(), attrs.end());
    out << "A" << attrs.size() << ":";
    for (const auto& [key, value] : attrs) out << key.size() << ":" << key << value.size() << ":" << value;
    out << "P" << graph.opOperands(op).size() << ":";
    for (const auto operand : operands) out << operand << ";";
    return out.str();
}

class ExpressionInterner {
public:
    explicit ExpressionInterner(const Graph& graph) : graph_(graph) {}

    ExpressionId valueKey(ValueId value) {
        if (const auto found = memo_.find(value); found != memo_.end()) return found->second;
        if (!value) return identityKey(value);
        frames_.push_back({value, {}, 0, operands_.size(), false});
        while (!frames_.empty()) {
            auto& frame = frames_.back();
            if (!frame.entered) {
                frame.entered = true;
                active_.insert(frame.value);
                frame.op = graph_.valueDef(frame.value);
                if (!frame.op || graph_.opResults(frame.op).size() != 1 ||
                    (graph_.opKind(frame.op) != OperationKind::kConstant &&
                     operationEffect(graph_, frame.op) != OpEffect::Pure)) {
                    finish(identityKey(frame.value));
                    continue;
                }
            }
            const auto inputs = graph_.opOperands(frame.op);
            if (frame.nextOperand < inputs.size()) {
                const auto input = inputs[frame.nextOperand];
                if (const auto found = memo_.find(input); found != memo_.end()) {
                    operands_.push_back(found->second);
                    ++frame.nextOperand;
                } else if (!input || active_.contains(input)) {
                    // Preserve the previous cycle fallback: a back edge is
                    // keyed by ValueId without memoizing it as the value's
                    // completed expression.
                    operands_.push_back(identityKey(input));
                    ++frame.nextOperand;
                } else {
                    frames_.push_back({input, {}, 0, operands_.size(), false});
                }
                continue;
            }
            const auto ids = std::span<const ExpressionId>(operands_).subspan(frame.operandsBegin);
            finish(intern(opKey(graph_, frame.op, ids)));
        }
        return memo_.at(value);
    }

private:
    struct Frame {
        ValueId value;
        OperationId op;
        std::size_t nextOperand;
        std::size_t operandsBegin;
        bool entered;
    };

    const Graph& graph_;
    // Each exact, shallow signature is owned once. Hash collisions still use
    // full string equality; child IDs encode ordered structural equivalence.
    std::unordered_map<std::string, ExpressionId> expressions_;
    std::unordered_map<ValueId, ExpressionId, ValueIdHash> memo_;
    std::unordered_set<ValueId, ValueIdHash> active_;
    std::vector<Frame> frames_;
    std::vector<ExpressionId> operands_;

    ExpressionId intern(std::string key) {
        const auto nextId = expressions_.size();
        return expressions_.try_emplace(std::move(key), nextId).first->second;
    }

    ExpressionId identityKey(ValueId value) {
        return intern("V" + std::to_string(value.index) + ":" + std::to_string(value.generation));
    }

    void finish(ExpressionId id) {
        const auto frame = frames_.back();
        memo_.emplace(frame.value, id);
        active_.erase(frame.value);
        operands_.resize(frame.operandsBegin);
        frames_.pop_back();
    }
};

struct Entry {
    OperationId op;
    ValueId value;
    bool multipleConsumers;
};

std::size_t shareCommonExpressions(Graph& graph)
{
    std::unordered_map<ExpressionId, std::vector<Entry>> groups;
    groups.reserve(graph.operations().size());
    ExpressionInterner interner(graph);
    for (const auto op : graph.operations()) {
        if (!pureCandidate(graph, op)) continue;
        const auto result = graph.opResults(op)[0];
        const auto users = graph.valueUsers(result);
        const bool multipleConsumers = !users.empty() && std::any_of(users.begin(), users.end(),
            [&](const auto& user) { return user.operation != users.front().operation; });
        // GSIM counts distinct consumer nodes before applying any aliases.
        groups[interner.valueKey(result)].push_back({op, result, multipleConsumers});
    }

    std::size_t shared = 0;
    for (auto& [key, entries] : groups) {
        (void)key;
        if (entries.size() < 2) continue;
        const auto width = graph.valueWidth(entries.front().value);
        bool merge = entries.size() >= kMinimumCommonExpressions || width > kBasicWidth;
        for (const auto& entry : entries)
            merge = merge || entry.multipleConsumers;
        if (!merge) continue;
        const auto representative = entries.front();
        for (std::size_t i = 1; i < entries.size(); ++i) {
            const auto duplicate = entries[i];
            if (!graph.opResults(duplicate.op).empty() && graph.valueDef(duplicate.value) == duplicate.op) {
                graph.mergeOpSemanticIds(representative.op, duplicate.op);
                graph.mergeValueSemanticIds(representative.value, duplicate.value);
                if (const auto origins = getOriginModules(graph, duplicate.op))
                    mergeOriginModules(graph, representative.op, *origins);
                ingest::node::mergeCseOperations(graph, representative.op, duplicate.op);
                graph.replaceAllUses(duplicate.value, representative.value);
                // Keep the duplicate operation/value rows for the caller's
                // explicit DCE boundary, matching ExprOpt/UsedBits/SplitNodes
                // and making this pass's graph delta independently observable.
                ++shared;
            }
        }
    }
    return shared;
}

} // namespace

CommonExprPass::CommonExprPass(CommonExprOptions options)
    : Pass("common-expr", "common-expr", "GSIM-compatible common expression sharing on ordinary GRH"),
      options_(std::move(options))
{}

bool CommonExprPass::supportsDialect(std::string_view dialect) const noexcept
{
    return dialect == "grh";
}

PassResult CommonExprPass::run()
{
    PassResult result;
    try {
        Graph& graph = [&]() -> Graph& {
            if (options_.top.empty()) {
                if (design().topGraphs().size() != 1)
                    throw std::invalid_argument("Select exactly one top graph with -top");
                return *design().findGraph(design().topGraphs()[0]);
            }
            auto* selected = design().findGraph(options_.top);
            if (!selected) throw std::invalid_argument("Top graph does not exist: " + options_.top);
            return *selected;
        }();
        requireWholeGrh(graph);
        verifyWhole(graph);
        const auto shared = shareCommonExpressions(graph);
        result.changed = shared != 0;
        verifyWhole(graph);
        const auto summary = "shared=" + std::to_string(shared) +
            " operations=" + std::to_string(graph.operations().size());
        info(summary);
        logInfo(summary);
    } catch (const std::exception& error) {
        this->error(error.what());
        result.failed = true;
    }
    return result;
}

} // namespace wolvrix::lib::transform
