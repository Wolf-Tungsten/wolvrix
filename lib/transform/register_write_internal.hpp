#ifndef WOLVRIX_REGISTER_WRITE_INTERNAL_HPP
#define WOLVRIX_REGISTER_WRITE_INTERNAL_HPP

#include "core/grh.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace wolvrix::lib::transform::write_detail
{
    namespace grh = wolvrix::lib::grh;
    using grh::Graph;
    using grh::OperationId;
    using grh::OperationKind;
    using grh::ValueId;

    inline ValueId strip(const Graph &graph, ValueId value)
    {
        for (int depth = 0; depth < 128 && value.valid(); ++depth)
        {
            const auto def = graph.valueDef(value);
            if (!def.valid()) break;
            const auto operands = graph.opOperands(def);
            if ((graph.opKind(def) != OperationKind::kAssign &&
                 graph.opKind(def) != OperationKind::kConcat) || operands.size() != 1 ||
                graph.valueWidth(operands[0]) != graph.valueWidth(value)) break;
            value = operands[0];
        }
        return value;
    }

    inline std::optional<uint64_t> constant(const Graph &graph, ValueId value)
    {
        value = strip(graph, value);
        const auto def = graph.valueDef(value);
        if (!def.valid() || graph.opKind(def) != OperationKind::kConstant) return std::nullopt;
        const auto attr = graph.getOperation(def).attr("constValue");
        const auto *literal = attr ? std::get_if<std::string>(&*attr) : nullptr;
        if (!literal) return std::nullopt;
        std::string text;
        for (unsigned char ch : *literal)
            if (ch != '_' && !std::isspace(ch)) text.push_back(static_cast<char>(std::tolower(ch)));
        int base = 10;
        auto tick = text.find('\'');
        std::size_t start = 0;
        if (tick != std::string::npos)
        {
            start = tick + 1;
            if (start >= text.size()) return std::nullopt;
            switch (text[start])
            {
            case 'b': base = 2; ++start; break;
            case 'o': base = 8; ++start; break;
            case 'd': base = 10; ++start; break;
            case 'h': base = 16; ++start; break;
            case '0': if (start + 1 == text.size()) return 0; break;
            default: return std::nullopt;
            }
        }
        if (start == text.size()) return std::nullopt;
        uint64_t result = 0;
        for (; start < text.size(); ++start)
        {
            const char ch = text[start];
            const unsigned digit = ch >= '0' && ch <= '9' ? ch - '0' :
                                   ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : 255;
            if (digit >= static_cast<unsigned>(base) || result > (UINT64_MAX - digit) / base)
                return std::nullopt;
            result = result * base + digit;
        }
        return result;
    }

    // Intern Boolean gates so row substitution produces shared, comparable
    // guards even when ingest emitted separately named copies of each gate.
    class BooleanBuilder
    {
    public:
        explicit BooleanBuilder(Graph &graph) : graph_(graph) {}

        ValueId literal(bool bit)
        {
            auto &value = literals_[bit];
            if (!value.valid())
            {
                value = graph_.createValue(1, false);
                const auto op = graph_.createOperation(OperationKind::kConstant, graph_.makeInternalOpSym());
                graph_.addResult(op, value);
                graph_.setAttr(op, "constValue", std::string(bit ? "1'b1" : "1'b0"));
            }
            return value;
        }

        ValueId negate(ValueId value)
        {
            value = strip(graph_, value);
            if (const auto bit = constant(graph_, value)) return literal(*bit == 0);
            const auto def = graph_.valueDef(value);
            if (def.valid() && (graph_.opKind(def) == OperationKind::kLogicNot ||
                                graph_.opKind(def) == OperationKind::kNot) &&
                graph_.opOperands(def).size() == 1) return strip(graph_, graph_.opOperands(def)[0]);
            return gate(OperationKind::kNot, value, {});
        }

        ValueId combine(OperationKind kind, ValueId a, ValueId b)
        {
            a = strip(graph_, a);
            b = strip(graph_, b);
            if (a == b) return a;
            const auto ca = constant(graph_, a);
            const auto cb = constant(graph_, b);
            if (kind == OperationKind::kAnd)
            {
                if ((ca && !*ca) || (cb && !*cb)) return literal(false);
                if (ca && *ca == 1) return b;
                if (cb && *cb == 1) return a;
            }
            if (kind == OperationKind::kOr)
            {
                if ((ca && *ca == 1) || (cb && *cb == 1)) return literal(true);
                if (ca && !*ca) return b;
                if (cb && !*cb) return a;
            }
            if (b.index < a.index) std::swap(a, b);
            return gate(kind, a, b);
        }

        ValueId gate(OperationKind kind, ValueId a, ValueId b)
        {
            const auto key = std::tuple{kind, a.index, b.index};
            if (const auto it = gates_.find(key); it != gates_.end()) return it->second;
            const auto result = graph_.createValue(1, false);
            const auto op = graph_.createOperation(kind, graph_.makeInternalOpSym());
            graph_.addOperand(op, a);
            if (b.valid()) graph_.addOperand(op, b);
            graph_.addResult(op, result);
            gates_.emplace(key, result);
            return result;
        }

    private:
        Graph &graph_;
        ValueId literals_[2];
        std::map<std::tuple<OperationKind, uint32_t, uint32_t>, ValueId> gates_;
    };

    using Literal = std::pair<ValueId, bool>;
    using Facts = std::unordered_map<ValueId, bool, grh::ValueIdHash>;

    // A bounded conjunction proof. Recording a compound atom as well as its
    // implied children proves if/else guards disjoint without expanding DNF.
    inline bool addFacts(const Graph &graph, ValueId value, bool positive,
                         Facts &facts, unsigned &budget)
    {
        if (budget == 0) return true; // no proof, never invent a contradiction
        --budget;
        value = strip(graph, value);
        if (const auto c = constant(graph, value)) return (*c != 0) == positive;
        const auto [it, inserted] = facts.emplace(value, positive);
        if (!inserted) return it->second == positive;
        const auto def = graph.valueDef(value);
        if (!def.valid() || graph.valueWidth(value) != 1) return true;
        const auto kind = graph.opKind(def);
        const auto operands = graph.opOperands(def);
        if ((kind == OperationKind::kNot || kind == OperationKind::kLogicNot) && operands.size() == 1)
            return addFacts(graph, operands[0], !positive, facts, budget);
        if (operands.size() == 2 &&
            ((positive && (kind == OperationKind::kAnd || kind == OperationKind::kLogicAnd)) ||
             (!positive && (kind == OperationKind::kOr || kind == OperationKind::kLogicOr))))
            return addFacts(graph, operands[0], positive, facts, budget) &&
                   addFacts(graph, operands[1], positive, facts, budget);
        return true;
    }

    inline std::optional<bool> implied(const Graph &graph, ValueId value, const Facts &facts,
                                      unsigned &budget, unsigned depth = 0, bool skipFact = false)
    {
        if (!budget || depth > 128) return std::nullopt;
        --budget;
        value = strip(graph, value);
        if (const auto c = constant(graph, value)) return *c != 0;
        if (!skipFact)
            if (const auto it = facts.find(value); it != facts.end()) return it->second;
        const auto def = graph.valueDef(value);
        if (!def.valid() || graph.valueWidth(value) != 1) return std::nullopt;
        const auto kind = graph.opKind(def);
        const auto args = graph.opOperands(def);
        if ((kind == OperationKind::kNot || kind == OperationKind::kLogicNot) && args.size() == 1)
        {
            if (auto bit = implied(graph, args[0], facts, budget, depth + 1)) return !*bit;
        }
        if (args.size() != 2) return std::nullopt;
        const bool isAnd = kind == OperationKind::kAnd || kind == OperationKind::kLogicAnd;
        const bool isOr = kind == OperationKind::kOr || kind == OperationKind::kLogicOr;
        if (!isAnd && !isOr) return std::nullopt;
        const auto lhs = implied(graph, args[0], facts, budget, depth + 1);
        const auto rhs = implied(graph, args[1], facts, budget, depth + 1);
        if (isAnd && ((lhs && !*lhs) || (rhs && !*rhs))) return false;
        if (isOr && ((lhs && *lhs) || (rhs && *rhs))) return true;
        if (lhs && rhs) return isAnd ? *lhs && *rhs : *lhs || *rhs;
        return std::nullopt;
    }

    inline bool contradictory(const Graph &graph, const std::vector<Literal> &terms)
    {
        Facts facts;
        unsigned budget = 512;
        for (const auto &[value, positive] : terms)
            if (!addFacts(graph, value, positive, facts, budget)) return true;
        // DNF may expose a conjunction's children while retaining its negation
        // as one atom: a && b && !(a && b). Check those compound facts too.
        budget = 2048;
        for (const auto &[value, positive] : facts)
            if (const auto bit = implied(graph, value, facts, budget, 0, true); bit && *bit != positive)
                return true;
        return false;
    }

    inline OperationId copyWrite(Graph &graph, const grh::Operation &source,
                                 const std::vector<ValueId> &operands)
    {
        const auto result = graph.createOperation(source.kind(), graph.makeInternalOpSym());
        for (auto value : operands) graph.addOperand(result, value);
        for (const auto &attr : source.attrs()) graph.setAttr(result, attr.key, attr.value);
        if (source.srcLoc()) graph.setOpSrcLoc(result, *source.srcLoc());
        return result;
    }
}

#endif
