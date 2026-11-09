#ifndef WOLVRIX_CORE_BEHAVIOR_STATE_GROUP_HPP
#define WOLVRIX_CORE_BEHAVIOR_STATE_GROUP_HPP

#include "core/grh.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::grh {
// One commit sink owns the ordered writes to a private kMemory backing store.
// Operands: enable, synchronous reset, (valid,index,data)*N, clock.
// Read ports remain ordinary memory sources; they never depend on next-state.
struct BehaviorStateLayout {
    std::vector<int64_t> rows, widths, offsets, updateArrays;
    std::vector<std::string> names, initial;
    int64_t totalRows = 0, width = 0;
};
template<class T> inline T behaviorAttr(const Operation& op, std::string_view key) {
    auto attr = op.attr(key);
    if (!attr || !std::holds_alternative<T>(*attr))
        throw std::runtime_error("behavior group missing/invalid attribute " + std::string(key));
    return std::get<T>(*attr);
}
inline BehaviorStateLayout behaviorStateLayout(const Graph& graph, const Operation& op) {
    if (op.kind() != OperationKind::kBehaviorStateGroup || !op.results().empty() ||
        behaviorAttr<int64_t>(op,"behavior.version") != 1 ||
        !behaviorAttr<bool>(op,"behavior.observerOnly"))
        throw std::runtime_error("invalid behavior group contract/version");
    BehaviorStateLayout d;
    d.rows=behaviorAttr<std::vector<int64_t>>(op,"behavior.rows");
    d.widths=behaviorAttr<std::vector<int64_t>>(op,"behavior.widths");
    d.names=behaviorAttr<std::vector<std::string>>(op,"behavior.names");
    d.initial=behaviorAttr<std::vector<std::string>>(op,"behavior.initial");
    d.updateArrays=behaviorAttr<std::vector<int64_t>>(op,"behavior.updateArrays");
    if (d.rows.empty() || d.rows.size()!=d.widths.size() || d.rows.size()!=d.names.size())
        throw std::runtime_error("invalid behavior array layout");
    std::unordered_set<std::string> names;
    for (size_t i=0;i<d.rows.size();++i) {
        if (d.rows[i]<1 || d.rows[i]>65536 || !std::has_single_bit(uint64_t(d.rows[i])) ||
            d.widths[i]<1 || d.widths[i]>64 || !names.insert(d.names[i]).second)
            throw std::runtime_error("behavior v1 requires unique power-of-two arrays of 1..64-bit UInt");
        d.offsets.push_back(d.totalRows);d.totalRows+=d.rows[i];d.width=std::max(d.width,d.widths[i]);
    }
    if (d.totalRows>65536 || d.initial.size()!=size_t(d.totalRows))
        throw std::runtime_error("invalid behavior initialization/layout size");
    for (size_t a=0,k=0;a<d.rows.size();++a) for(int64_t row=0;row<d.rows[a];++row,++k) {
        size_t used=0;auto value=std::stoull(d.initial[k],&used,10);
        if (used!=d.initial[k].size() || (d.widths[a]<64 && value>=(uint64_t(1)<<d.widths[a])))
            throw std::runtime_error("behavior initializer exceeds element width");
    }
    auto in=op.operands();
    if (in.size()!=3+3*d.updateArrays.size() ||
        behaviorAttr<std::vector<std::string>>(op,"eventEdge")!=std::vector<std::string>{"posedge"})
        throw std::runtime_error("invalid behavior operand/event layout");
    for(auto v:{in[0],in[1],in.back()})
        if(graph.valueType(v)!=ValueType::Logic || graph.valueWidth(v)!=1)
            throw std::runtime_error("behavior enable/reset/clock must be one bit");
    for(size_t i=0;i<d.updateArrays.size();++i) {
        auto a=d.updateArrays[i];
        if(a<0 || size_t(a)>=d.rows.size()) throw std::runtime_error("invalid behavior update array");
        auto aw=std::max(1,int(std::bit_width(uint64_t(d.rows[a]-1))));
        if(graph.valueWidth(in[2+3*i])!=1 || graph.valueWidth(in[3+3*i])!=aw ||
            graph.valueWidth(in[4+3*i])!=d.widths[a])
            throw std::runtime_error("behavior update port width mismatch");
        for(auto v:{in[2+3*i],in[3+3*i],in[4+3*i]})
            if(graph.valueType(v)!=ValueType::Logic) throw std::runtime_error("behavior port must be logic");
    }
    return d;
}
inline void validateBehaviorOwnership(const Graph& graph) {
    std::unordered_set<std::string> owned;
    for (auto id:graph.operations()) {
        auto op=graph.getOperation(id);
        if(op.kind()==OperationKind::kBehaviorStateGroup) {
            behaviorStateLayout(graph,op);
            auto name=behaviorAttr<std::string>(op,"memSymbol");
            if(!owned.insert(name).second) throw std::runtime_error("duplicate behavior state owner");
        }
    }
    if(owned.empty()) return;
    struct ObserverState {
        bool allowed = false;
        std::vector<ValueId> reads;
    };
    auto isObserver = [](const Operation& op) {
        auto attr = op.attr("behavior.observerDomain");
        return attr && std::holds_alternative<std::string>(*attr) && !std::get<std::string>(*attr).empty();
    };
    std::unordered_map<std::string, ObserverState> states;
    std::vector<ValueId> todo;
    for(auto id:graph.operations()) {
        auto op=graph.getOperation(id);
        if (op.kind()==OperationKind::kRegister || op.kind()==OperationKind::kLatch || op.kind()==OperationKind::kMemory)
            states[std::string(op.symbolText())].allowed = isObserver(op);
        const char* readSymbol = op.kind()==OperationKind::kRegisterReadPort ? "regSymbol" :
            op.kind()==OperationKind::kLatchReadPort ? "latchSymbol" :
            op.kind()==OperationKind::kMemoryReadPort ? "memSymbol" : nullptr;
        if (readSymbol) {
            auto& readers = states[behaviorAttr<std::string>(op, readSymbol)].reads;
            readers.insert(readers.end(), op.results().begin(), op.results().end());
        }
        if(op.kind()==OperationKind::kMemoryReadPort && owned.contains(behaviorAttr<std::string>(op,"memSymbol")))
            todo.insert(todo.end(),op.results().begin(),op.results().end());
        if((op.kind()==OperationKind::kMemoryWritePort || op.kind()==OperationKind::kMemoryFillPort) &&
            owned.contains(behaviorAttr<std::string>(op,"memSymbol")))
            throw std::runtime_error("behavior backing store has an ordinary writer");
    }
    std::unordered_set<ValueId,ValueIdHash> seen;
    while(!todo.empty()) {
        auto value=todo.back();todo.pop_back();
        if(!seen.insert(value).second) continue;
        const auto current = graph.getValue(value);
        for(const auto& use:current.users()) {
            auto op=graph.getOperation(use.operation);
            if(op.kind()==OperationKind::kBehaviorStateGroup) continue;
            switch(op.kind()) {
            case OperationKind::kRegisterWritePort: case OperationKind::kLatchWritePort:
            case OperationKind::kMemoryWritePort: case OperationKind::kMemoryFillPort: {
                const char* attr = op.kind()==OperationKind::kRegisterWritePort ? "regSymbol" :
                    op.kind()==OperationKind::kLatchWritePort ? "latchSymbol" : "memSymbol";
                const auto& state = states.at(behaviorAttr<std::string>(op, attr));
                if (!state.allowed) throw std::runtime_error("observer group feeds unowned state/effect");
                // Continue across debug delay storage: a label must not hide a
                // later path back into functional state.
                todo.insert(todo.end(), state.reads.begin(), state.reads.end());
                break;
            }
            case OperationKind::kDpicCall: case OperationKind::kSystemTask:
                if (!isObserver(op) || !op.results().empty())
                    throw std::runtime_error("observer group feeds unowned state/effect");
                break;
            case OperationKind::kXMRWrite:
                throw std::runtime_error("observer group feeds unowned state/effect");
            default: break;
            }
            todo.insert(todo.end(),op.results().begin(),op.results().end());
        }
    }
}

}
#endif
