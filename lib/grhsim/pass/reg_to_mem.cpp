#include "grhsim/pass/reg_to_mem.hpp"

#include "grhsim/ir/model.hpp"
#include "slang/numeric/SVInt.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace wolvrix::lib::grhsim {
namespace {
    template<class T> const T *param(const GrhSimModel &m, std::span<const Parameter> ps, std::string_view name) {
        for (const auto &p : ps) if (m.text(p.name) == name) return std::get_if<T>(&p.value);
        return nullptr;
    }
    struct Guard {
        ValueId address;
        uint64_t row = 0;
        std::vector<ValueId> terms;
        struct Conflict { ValueId address; std::vector<ValueId> terms; };
        std::vector<Conflict> conflicts;
    };
    struct Branch { Guard guard; ValueId data; };
    struct RowWrite {
        OpId op;
        uint64_t row = 0;
        std::vector<Branch> branches; // Original mux order, highest first.
        ValueId mask;
        std::vector<ValueId> events;
        std::vector<std::string> edges;
        std::vector<StateId> histories;
        std::string key;
    };
    struct Group {
        std::vector<StateId> rows;
        uint64_t base = 0;
        bool writeSource = false;
        bool indexedRead = false;
        bool selected = false;
        bool mergeWrites = false;
        int64_t writeSavings = 0, readSavings = 0, combinedSavings = 0;
        std::vector<RowWrite> writes;
        std::vector<std::size_t> order; // Low to high.
        std::string reason;
        std::string detail;
    };
    struct PackedSlice {
        OpId op;
        ValueId index;
        uint64_t offset = 0;
        bool bitWindow = false;
    };
    struct PackedView {
        ValueId packed;
        std::vector<StateId> lanes; // Low packed lane first, including duplicates.
        std::vector<PackedSlice> slices;
        bool selected = false;
    };
    struct LaneRun {
        uint32_t begin, end, row;
        bool repeated;
    };

    // Express edge repetitions and overlapping views as a few contiguous runs.
    // A run is either one repeated row or sequential rows with table wraparound.
    std::vector<LaneRun> laneRuns(const std::vector<uint32_t> &rows, uint64_t count) {
        std::vector<LaneRun> runs;
        for (uint32_t begin = 0; begin < rows.size();) {
            uint32_t end = begin + 1;
            const bool repeated = end < rows.size() && rows[end] == rows[begin];
            while (end < rows.size() && rows[end] ==
                (repeated ? rows[begin] : (rows[begin] + uint64_t{end - begin}) % count)) ++end;
            runs.push_back({begin, end, rows[begin], repeated});
            begin = end;
        }
        return runs;
    }

    class Engine {
    public:
        Engine(GrhSimModel &model, const RegToMemOptions &options) : m(model), options(options) {
            defs.resize(m.values().size()+1);
            valueUses.resize(m.values().size()+1);
            canonical.resize(m.values().size()+1);
            refs.resize(m.states().size()+1);
            writers.resize(m.states().size()+1);
            inits.resize(m.states().size()+1);
            eligibility.resize(m.states().size()+1);
            for (const auto &op : m.operations()) {
                for (auto v : m.results(op)) defs[v.index] = op.id;
                for (auto v : m.operands(op)) ++valueUses[v.index];
                const auto objects = m.objectRefs(op);
                for (auto ref : objects) if (ref.kind == ObjectKind::State) refs[ref.index].push_back(op.id);
                if (kind(op) == "core.state.regWrite" && !objects.empty()) writers[objects[0].index].push_back(op.id);
            }
            for (const auto &init : m.initRecords()) inits[init.state.index] = &init;
            std::unordered_map<uint32_t,std::set<uint64_t>> decodedRows;
            for (const auto &op:m.operations()) if(m.results(op).size()==1 &&
                (kind(op)=="core.compute.eq" || kind(op)=="core.compute.reduceAnd" ||
                 kind(op)=="core.compute.logicNot" || kind(op)=="core.compute.reduceNor")) {
                if(auto eq=equality(m.results(op)[0])) {
                    auto &rows=decodedRows[eq->first.index];
                    if(rows.size()<options.minElementCount) rows.insert(eq->second);
                }
            }
            for(const auto &[address,rows]:decodedRows)
                if(rows.size()>=options.minElementCount) decodedAddresses.insert(address);
        }
        std::string_view kind(const SimOp &op) const { return m.text(op.opType); }
        const Type &type(ValueId v) const { return m.types()[m.values()[v.index-1].type.index-1]; }
        TypeId typeId(ValueId v) const { return m.values()[v.index-1].type; }
        const SimOp *def(ValueId v) const {
            return v.index < defs.size() && defs[v.index] ? &m.operations()[defs[v.index].index-1] : nullptr;
        }
        ValueId unwrap(ValueId v) {
            if (v.index >= canonical.size()) return v;
            if (canonical[v.index]) return canonical[v.index];
            std::vector<ValueId> path;
            auto current = v;
            for (unsigned depth=0; depth<4096; ++depth) {
                if (canonical[current.index]) { current=canonical[current.index]; break; }
                const auto *op=def(current);
                if (!op || kind(*op)!="core.compute.assign" || m.operands(*op).size()!=1 ||
                    typeId(current)!=typeId(m.operands(*op)[0])) break;
                path.push_back(current); current=m.operands(*op)[0];
            }
            canonical[v.index]=current;
            for (auto x:path) canonical[x.index]=current;
            return current;
        }
        std::optional<slang::SVInt> literal(ValueId v) {
            v=unwrap(v); const auto *op=def(v);
            if (!op || kind(*op)!="core.compute.constant" || type(v).kind!=TypeKind::Logic) return {};
            const auto ps=m.parameters(*op);
            const auto *s=param<std::string>(m,ps,"value");
            if (!s) s=param<std::string>(m,ps,"constValue");
            if (!s) return {};
            try {
                bool negative=!s->empty() && s->front()=='-';
                auto n=slang::SVInt::fromString(negative ? s->substr(1) : *s);
                if(negative) n=-n;
                return n.resize(type(v).width);
            } catch (const std::exception &) { return {}; }
        }
        std::optional<uint64_t> number(ValueId v) {
            auto n=literal(v); if (!n || n->hasUnknown() || n->getBitWidth()>64) return {};
            return n->getRawPtr()[0];
        }
        bool ones(ValueId v) {
            auto n=literal(v); if (!n || n->hasUnknown()) return false;
            return n->countOnes()==type(v).width;
        }
        std::string key(ValueId v) {
            v=unwrap(v);
            if (const auto *op=def(v); op && kind(*op)=="core.compute.constant") {
                if (auto n=literal(v)) return "c"+std::to_string(typeId(v).index)+":"+n->toString(slang::LiteralBase::Hex,false);
            }
            return "v"+std::to_string(v.index);
        }
        void flatten(ValueId v, bool conjunction, std::vector<ValueId> &out) {
            std::vector<ValueId> stack{v}; std::unordered_set<uint32_t> seen;
            while(!stack.empty() && seen.size()<16384) {
                auto x=unwrap(stack.back()); stack.pop_back(); if(!seen.insert(x.index).second) continue;
                const auto *op=def(x); const auto k=op?kind(*op):std::string_view{};
                if(op && (k==(conjunction?"core.compute.logicAnd":"core.compute.logicOr") ||
                          (type(x).width==1 && k==(conjunction?"core.compute.and":"core.compute.or")))) {
                    for(auto y:m.operands(*op)) stack.push_back(y);
                } else out.push_back(x);
            }
            if(!stack.empty()) { out.clear(); out.push_back(unwrap(v)); }
            std::sort(out.begin(),out.end(),[](auto a,auto b){return a.index<b.index;});
            out.erase(std::unique(out.begin(),out.end()),out.end());
        }
        std::optional<std::pair<ValueId,uint64_t>> equality(ValueId v) {
            v=unwrap(v); const auto *op=def(v);
            if(op && m.operands(*op).size()==1) {
                const auto a=unwrap(m.operands(*op)[0]);
                const auto &t=type(a);
                if(t.kind==TypeKind::Logic && !t.isSigned && t.width<=64) {
                    if(kind(*op)=="core.compute.reduceAnd")
                        return std::pair{a,t.width==64?~uint64_t{0}:(uint64_t{1}<<t.width)-1};
                    if(kind(*op)=="core.compute.logicNot" || kind(*op)=="core.compute.reduceNor")
                        return std::pair{a,uint64_t{0}};
                }
            }
            if(!op || kind(*op)!="core.compute.eq" || m.operands(*op).size()!=2) return {};
            auto a=unwrap(m.operands(*op)[0]), b=unwrap(m.operands(*op)[1]);
            if(auto n=number(b); n && !number(a) && type(a).width==type(b).width && !type(a).isSigned)
                return std::pair{a,*n};
            if(auto n=number(a); n && !number(b) && type(a).width==type(b).width && !type(b).isSigned)
                return std::pair{b,*n};
            return {};
        }
        ValueId negated(ValueId v) {
            v=unwrap(v); const auto *op=def(v);
            if(op && (kind(*op)=="core.compute.logicNot" || (kind(*op)=="core.compute.not" && type(v).width==1)) &&
                m.operands(*op).size()==1) return unwrap(m.operands(*op)[0]);
            return {};
        }
        std::optional<Guard> guard(ValueId value, std::optional<uint64_t> row={}) {
            Guard result; std::vector<ValueId> terms; flatten(value,true,terms);
            for(auto t:terms) if(auto e=equality(t); e && decodedAddresses.contains(e->first.index) && (!row || e->second==*row)) {
                if(result.address) return {};
                result.address=e->first; result.row=e->second;
            }
            for(auto t:terms) {
                if(auto e=equality(t); e && result.address==e->first && result.row==e->second) continue;
                if(auto n=number(t); n && *n==1 && type(t).width==1) continue;
                if(auto inner=negated(t); inner && result.address) {
                    std::vector<ValueId> blocked; flatten(inner,true,blocked);
                    Guard::Conflict conflict; bool invalid=false;
                    for(auto b:blocked) {
                        if(auto eq=equality(b); eq && decodedAddresses.contains(eq->first.index) && eq->second==result.row) {
                            if(conflict.address) invalid=true;
                            conflict.address=eq->first;
                        } else conflict.terms.push_back(b);
                    }
                    if(conflict.address && !invalid) { result.conflicts.push_back(std::move(conflict)); continue; }
                }
                result.terms.push_back(t);
            }
            return result;
        }
        std::string termsKey(const std::vector<ValueId> &terms) {
            std::string result; for(auto t:terms) result+=key(t)+","; return result;
        }
        std::string guardKey(const Guard &g) {
            std::string result=key(g.address)+"["+termsKey(g.terms)+"]";
            for(const auto &c:g.conflicts) result+="!"+key(c.address)+"["+termsKey(c.terms)+"]";
            return result;
        }
        bool eligible(StateId s) {
            auto &cached=eligibility[s.index];
            if(cached) return cached>0;
            const auto reject=[&](std::string_view reason) {
                cached=-1;
                auto &entry=excludedStates[std::string(reason)];
                ++entry.first;
                if(entry.second.empty()) entry.second=m.text(m.states()[s.index-1].name);
                return false;
            };
            const auto &t=m.types()[m.states()[s.index-1].type.index-1];
            if(t.kind!=TypeKind::Logic || t.domain!=LogicDomain::TwoState) return reject("type");
            const auto *init=inits[s.index]; if(!init || m.steps(*init).size()!=1) return reject("init");
            const auto &step=m.steps(*init)[0];
            if(m.text(step.kind)!="core.init.const" || !param<std::string>(m,m.parameters(step),"value")) return reject("init");
            for(auto id:refs[s.index]) {
                const auto &op=m.operations()[id.index-1]; const auto objects=m.objectRefs(op);
                if(objects.empty() || objects[0]!=ObjectRef::state(s) ||
                    (kind(op)!="core.state.read" && kind(op)!="core.state.regWrite")) return reject("unknown-state-user");
                // A storage state cannot also be an event-history operand.
                if(std::count(objects.begin(),objects.end(),ObjectRef::state(s))!=1) return reject("history-owner");
            }
            cached=1;
            return true;
        }
        std::optional<RowWrite> parseWrite(StateId s, std::optional<uint64_t> row={}) {
            parseReason="write-shape";
            if(writers[s.index].size()!=1) return {};
            const auto &op=m.operations()[writers[s.index][0].index-1];
            auto operands=m.operands(op); auto objects=m.objectRefs(op);
            auto edges=param<std::vector<std::string>>(m,m.parameters(op),"event_edges");
            if(!edges || edges->empty() || operands.size()!=3+edges->size() || objects.size()!=1+edges->size()) return {};
            RowWrite result; result.op=op.id; result.mask=unwrap(operands[2]); result.edges=*edges;
            for(std::size_t i=0;i<edges->size();++i) {
                result.events.push_back(unwrap(operands[3+i])); result.histories.push_back({objects[1+i].index,0});
            }
            // A regWrite carries a global update condition separately from the
            // mux condition in some lowering forms.  Keep it as part of every
            // effective branch guard; otherwise a false updateCond would still
            // commit a recovered memory write.
            const ValueId updateCond = unwrap(operands[0]);
            const bool always=number(updateCond)==std::optional<uint64_t>{1};
            std::vector<std::pair<ValueId,ValueId>> branches;
            auto current=unwrap(operands[1]); std::unordered_set<uint32_t> seen;
            while(seen.insert(current.index).second && branches.size()<2048) {
                const auto *mux=def(current);
                if(!mux || kind(*mux)!="core.compute.mux" || m.operands(*mux).size()!=3) break;
                const auto a=m.operands(*mux);
                if(typeId(a[1])!=typeId(current) || typeId(a[2])!=typeId(current)) return {};
                branches.emplace_back(unwrap(a[0]),unwrap(a[1])); current=unwrap(a[2]);
            }
            std::vector<ValueId> updates; flatten(updateCond,false,updates);
            const auto *fallback=def(current);
            const bool hold=fallback && kind(*fallback)=="core.state.read" && m.objectRefs(*fallback)[0]==ObjectRef::state(s);
            if(branches.empty()) {
                if(hold) return {};
                branches.emplace_back(updateCond,current);
            } else if(!hold) {
                // The fallback is unreachable only if every update disjunct
                // implies at least one mux condition. Otherwise keep it at
                // lowest priority; planWrites must prove overlap is legal.
                const auto covered=[&](ValueId u) {
                    if(std::any_of(branches.begin(),branches.end(),[&](const auto &branch) {return u==branch.first;}))
                        return true;
                    std::vector<ValueId> assumptions{u}; flatten(u,true,assumptions);
                    return std::any_of(branches.begin(),branches.end(),[&](const auto &branch) {
                        return implies(assumptions,branch.first);
                    });
                };
                std::vector<ValueId> fallbackUpdates;
                for(auto u:updates) if(!covered(u)) fallbackUpdates.push_back(u);
                for(auto u:fallbackUpdates) branches.emplace_back(u,current);
            }
            for(const auto &[condition,data]:branches) {
                auto g=guard(condition,row); if(!g) {parseReason="ambiguous-address";return {};}
                // If the mux branch only carries the address hit and the
                // regWrite update condition was separate, conjoin it here.
                // Avoid duplicating it when the branch already implies it.
                std::vector<ValueId> assumptions{condition}; flatten(condition,true,assumptions);
                const bool impliesUpdate=always || std::find(updates.begin(),updates.end(),condition)!=updates.end() ||
                    implies(assumptions,updateCond) ||
                    std::any_of(updates.begin(),updates.end(),[&](ValueId u) { return implies(assumptions,u); });
                if(!always && !impliesUpdate) {
                    bool present=std::find(g->terms.begin(),g->terms.end(),updateCond)!=g->terms.end();
                    if(!present) g->terms.push_back(updateCond);
                    std::sort(g->terms.begin(),g->terms.end(),[](auto a,auto b){return a.index<b.index;});
                    g->terms.erase(std::unique(g->terms.begin(),g->terms.end()),g->terms.end());
                }
                if(g->address) { if(row && g->row!=*row) {parseReason="row-map";return {};} row=g->row; }
                result.branches.push_back({std::move(*g),data});
            }
            if(!row) {parseReason="no-decoded-address";return {};}
            result.row=*row;
            result.key=std::to_string(m.states()[s.index-1].type.index)+"/"+key(result.mask);
            for(std::size_t i=0;i<result.events.size();++i) result.key+="/"+key(result.events[i])+result.edges[i];
            for(const auto &b:result.branches) result.key+="/"+guardKey(b.guard)+"="+key(b.data);
            return result;
        }

        void discover() {
            // Write-only tables do not need a packed read anchor.
            std::map<std::string,std::vector<std::pair<StateId,RowWrite>>> families;
            for(const auto &s:m.states()) if(eligible(s.id)) {
                if(auto row=parseWrite(s.id)) families[row->key].emplace_back(s.id,std::move(*row));
            }
            for(auto &[key,rows]:families) {
                (void)key;
                std::sort(rows.begin(),rows.end(),[](const auto &a,const auto &b){
                    return std::pair{a.second.row,a.first.index}<std::pair{b.second.row,b.first.index}; });
                Group g; g.writeSource=true;
                for(auto &[state,write]:rows) {
                    if(!g.rows.empty() && write.row!=g.base+g.rows.size()) {
                        if(g.rows.size()>=options.minElementCount) groups.push_back(std::move(g));
                        g=Group{}; g.writeSource=true;
                    }
                    if(g.rows.empty()) g.base=write.row;
                    g.rows.push_back(state); g.writes.push_back(std::move(write));
                }
                if(g.rows.size()>=options.minElementCount) groups.push_back(std::move(g));
            }
            // Recover storage from shared/repeated concat views as well as slices.
            std::map<std::vector<uint32_t>,std::size_t> layouts;
            std::unordered_map<uint32_t,std::vector<PackedSlice>> slices;
            for(const auto &op:m.operations()) {
                const auto k=kind(op);
                const auto args=m.operands(op);
                if(m.results(op).size()!=1) continue;
                PackedSlice slice{op.id};
                ValueId packed;
                if((k=="core.compute.sliceArray" || k=="core.compute.sliceDynamic") && args.size()==2) {
                    packed=unwrap(args[0]); slice.index=unwrap(args[1]);
                    slice.bitWindow=k=="core.compute.sliceDynamic";
                } else if(k=="core.compute.sliceStatic" && args.size()==1) {
                    const auto *shift=def(unwrap(args[0]));
                    const auto *start=param<int64_t>(m,m.parameters(op),"sliceStart");
                    if(!shift || kind(*shift)!="core.compute.lshr" || m.operands(*shift).size()!=2 ||
                        !start || *start<0) continue;
                    // The shift must retain the complete packed source width.
                    const auto sa=m.operands(*shift);
                    if(type(args[0]).width!=type(sa[0]).width) continue;
                    packed=unwrap(sa[0]); slice.index=unwrap(sa[1]);
                    slice.offset=static_cast<uint64_t>(*start);slice.bitWindow=true;
                } else continue;
                const auto &index=type(slice.index);
                if(index.kind!=TypeKind::Logic || index.isSigned || index.domain!=LogicDomain::TwoState || index.width>64) continue;
                slices[packed.index].push_back(slice);
            }
            for(const auto &op:m.operations()) if(kind(op)=="core.compute.concat") {
                std::vector<StateId> rows; bool ok=true; TypeId element;
                std::vector<StateId> lanes;
                std::vector<ValueId> stack(m.operands(op).begin(),m.operands(op).end());
                std::unordered_set<uint32_t> unique;
                while(!stack.empty() && lanes.size()<65536) {
                    auto v=unwrap(stack.back()); stack.pop_back(); const auto *d=def(v);
                    if(d && kind(*d)=="core.compute.concat") {
                        auto a=m.operands(*d); stack.insert(stack.end(),a.begin(),a.end()); continue;
                    }
                    if(!d || kind(*d)!="core.state.read" || m.objectRefs(*d).size()!=1) {ok=false;break;}
                    StateId s{m.objectRefs(*d)[0].index,0};
                    if(!eligible(s) || (element && element!=m.states()[s.index-1].type)) {ok=false;break;}
                    element=m.states()[s.index-1].type;
                    lanes.push_back(s);
                    if(unique.insert(s.index).second) rows.push_back(s);
                }
                if(!stack.empty() || !ok || rows.size()<options.minElementCount) continue;
                PackedView view{m.results(op)[0],std::move(lanes),{}};
                for(auto candidate:slices[view.packed.index]) {
                    const auto &slice=m.operations()[candidate.op.index-1];
                    const auto output=m.results(slice)[0];
                    if(candidate.bitWindow) {
                        if(type(output).kind!=TypeKind::Logic || type(output).width>64 ||
                            type(output).domain!=LogicDomain::TwoState) continue;
                    } else if(typeId(output)!=element) continue;
                    view.slices.push_back(candidate);
                }
                std::unordered_map<uint32_t,uint32_t> storageRows;
                for(uint32_t i=0;i<rows.size();++i) storageRows.emplace(rows[i].index,i);
                std::vector<uint32_t> mappedLanes;
                for(auto state:view.lanes) mappedLanes.push_back(storageRows.at(state.index));
                // Bound address-selection work independently of table depth.
                const bool indexed=!view.slices.empty() && laneRuns(mappedLanes,rows.size()).size()<=8;
                if(!view.slices.empty()) packedViews.push_back(std::move(view));
                std::vector<uint32_t> ids; for(auto s:rows) ids.push_back(s.index);
                auto [layout,inserted]=layouts.emplace(ids,groups.size());
                if(!inserted) {groups[layout->second].indexedRead |= indexed;continue;}
                Group g; g.rows=std::move(rows);g.indexedRead=indexed;
                for(std::size_t i=0;i<g.rows.size();++i) {
                    auto w=parseWrite(g.rows[i],i); if(!w) {g.reason=parseReason;g.writes.clear();break;}
                    g.writes.push_back(std::move(*w));
                }
                if(!g.writes.empty()) for(const auto &w:g.writes) if(w.key!=g.writes[0].key) {
                    g.reason="write-family";
                    const auto &first=g.writes[0];
                    if(w.branches.size()!=first.branches.size()) g.reason="branch-shape";
                    else for(std::size_t j=0;j<w.branches.size();++j) {
                        if(key(w.branches[j].data)!=key(first.branches[j].data)) {g.reason="row-dependent-data";break;}
                        if(guardKey(w.branches[j].guard)!=guardKey(first.branches[j].guard)) {
                            g.reason="row-dependent-guard";
                            g.detail="branch="+std::to_string(j)+" first="+guardKey(first.branches[j].guard)+
                                " row="+std::to_string(w.row)+" guard="+guardKey(w.branches[j].guard);
                            break;
                        }
                    }
                    g.writes.clear();break;
                }
                groups.push_back(std::move(g));
            }
            for(std::size_t i=0;i<packedViews.size();++i)
                viewsByFirstState[packedViews[i].lanes[0].index].push_back(i);
        }

        bool implies(const std::vector<ValueId> &assumptions, ValueId value, bool truth=true) {
            std::unordered_set<uint32_t> positive,negative;
            for(auto a:assumptions) {
                positive.insert(unwrap(a).index);
                if(auto inner=negated(a)) negative.insert(inner.index);
            }
            std::unordered_map<uint64_t,bool> memo;
            unsigned budget=4096;
            std::function<bool(ValueId,bool,unsigned)> prove=[&](ValueId v,bool wanted,unsigned depth) {
                v=unwrap(v);
                if((wanted?positive:negative).contains(v.index)) return true;
                if(depth>64 || !budget) return false;
                const uint64_t cacheKey=uint64_t{v.index}*2+wanted;
                if(auto it=memo.find(cacheKey); it!=memo.end()) return it->second;
                --budget;
                const bool result=[&]() {
                    if(auto n=number(v); n && type(v).width==1) return (*n!=0)==wanted;
                    if(auto inner=negated(v)) return prove(inner,!wanted,depth+1);
                    const auto *op=def(v); if(!op) return false;
                    const auto k=kind(*op);
                    const bool isAnd=k=="core.compute.logicAnd" || (k=="core.compute.and" && type(v).width==1);
                    const bool isOr=k=="core.compute.logicOr" || (k=="core.compute.or" && type(v).width==1);
                    if(!isAnd && !isOr) return false;
                    const bool all=(isAnd==wanted);
                    for(auto a:m.operands(*op)) {
                        const bool proof=prove(a,wanted,depth+1);
                        if(all && !proof) return false;
                        if(!all && proof) return true;
                    }
                    return all;
                }();
                memo.emplace(cacheKey,result);
                return result;
            };
            return prove(value,truth,0);
        }
        bool exclusive(const Guard &a,const Guard &b) {
            for(auto term:b.terms) if(implies(a.terms,term,false)) return true;
            for(auto term:a.terms) if(implies(b.terms,term,false)) return true;
            return false;
        }
        bool subsetProof(const std::vector<ValueId> &from,const std::vector<ValueId> &to) {
            for(auto t:to) if(!implies(from,t)) return false;
            return true;
        }
        bool planWrites(Group &g) {
            g.order.clear();
            if(!options.enableWriteMerge || g.writes.size()!=g.rows.size()) return false;
            const auto &first=g.writes[0];
            const auto &element=m.types()[m.states()[g.rows[0].index-1].type.index-1];
            if(first.branches.size()>1 && !ones(first.mask) && element.width!=1) {g.reason="mask";return false;}
            if(element.width!=1 && !ones(first.mask) &&
                std::any_of(first.branches.begin(),first.branches.end(),[](const Branch &b) {return !b.guard.address;})) {
                g.reason="masked-fill";return false;
            }
            for(const auto &w:g.writes) {
                if(w.key!=first.key) {g.reason="write-family";return false;}
                if(w.branches.size()!=first.branches.size()) {g.reason="branch-shape";return false;}
                for(std::size_t i=0;i<w.histories.size();++i) {
                    auto h=w.histories[i], expected=first.histories[i];
                    if(refs[h.index].size()!=1 || refs[h.index][0]!=w.op) {g.reason="history-owner";return false;}
                    const auto *init=inits[h.index], *other=inits[expected.index];
                    if(!init || !other || m.steps(*init).size()!=1 || m.steps(*other).size()!=1) {g.reason="history-init";return false;}
                    const auto &a=m.steps(*init)[0], &b=m.steps(*other)[0];
                    const auto ap=m.parameters(a), bp=m.parameters(b);
                    if(a.kind!=b.kind || m.text(a.kind)!="core.init.const" || ap.size()!=bp.size()) {g.reason="history-init";return false;}
                    for(std::size_t j=0;j<ap.size();++j) if(ap[j].name!=bp[j].name || ap[j].value!=bp[j].value) {g.reason="history-init";return false;}
                }
            }
            const auto n=first.branches.size();
            std::vector<std::vector<uint8_t>> blocks(n,std::vector<uint8_t>(n));
            for(std::size_t i=0;i<n;++i) {
                const auto &gi=first.branches[i].guard;
                for(const auto &c:gi.conflicts) {
                    bool found=false;
                    for(std::size_t j=0;j<n;++j) {
                        const auto &gj=first.branches[j].guard;
                        if(i==j || c.address!=gj.address) continue;
                        auto a=gi.terms; a.insert(a.end(),c.terms.begin(),c.terms.end());
                        auto b=gi.terms; b.insert(b.end(),gj.terms.begin(),gj.terms.end());
                        if(subsetProof(a,gj.terms) && subsetProof(b,c.terms)) {blocks[i][j]=1;found=true;break;}
                    }
                    if(!found) {g.reason="priority-blocker";return false;}
                }
            }
            std::vector<std::vector<std::size_t>> successors(n);
            std::vector<std::size_t> incoming(n);
            for(std::size_t i=0;i<n;++i) for(std::size_t j=i+1;j<n;++j) {
                const auto &a=first.branches[i].guard, &b=first.branches[j].guard;
                if(exclusive(a,b)) continue;
                if(!a.address || !b.address) {
                    if(!a.address && !b.address && key(first.branches[i].data)==key(first.branches[j].data)) continue;
                    g.reason="fill-overlap"; return false;
                }
                if(blocks[i][j] && blocks[j][i]) {g.reason="priority-cycle";return false;}
                auto low=j, high=i;
                if(blocks[i][j]) {low=i;high=j;}
                successors[low].push_back(high); ++incoming[high];
            }
            std::set<std::size_t> ready;
            for(std::size_t i=0;i<n;++i) if(!incoming[i]) ready.insert(i);
            while(!ready.empty()) {
                auto i=*ready.begin();ready.erase(ready.begin());g.order.push_back(i);
                for(auto j:successors[i]) if(!--incoming[j]) ready.insert(j);
            }
            if(g.order.size()!=n) {g.reason="priority-cycle";g.order.clear();return false;}
            return true;
        }
        ValueId compute(std::string_view k,TypeId t,std::vector<ValueId> args,std::vector<Parameter> ps={}) {
            // Intern only operations created by this pass. Shared packed views
            // often request the same bounds/address expression many times.
            std::string signature=std::string(k)+"/"+std::to_string(t.index);
            for(auto arg:args) signature+="/"+std::to_string(arg.index);
            bool cacheable=true;
            for(const auto &p:ps) {
                const auto *value=std::get_if<std::string>(&p.value);
                if(!value) {cacheable=false;break;}
                signature+="/p"+std::to_string(p.name.index)+":"+std::to_string(value->size())+":"+*value;
            }
            if(cacheable) if(auto it=createdComputes.find(signature);it!=createdComputes.end()) return it->second;
            auto v=m.addValue(t); const std::array result{v}; m.addOperation(k,args,result,{},ps);
            if(cacheable) createdComputes.emplace(std::move(signature),v);
            return v;
        }
        ValueId constant(TypeId t,uint64_t n) {
            auto literal=std::to_string(m.types()[t.index-1].width)+"'d"+std::to_string(n);
            return compute("core.compute.constant",t,{},{{m.intern("value"),literal}});
        }
        ValueId conjunction(const std::vector<ValueId> &terms) {
            if(terms.empty()) return constant(bit,1);
            auto v=terms.front();
            for(std::size_t i=1;i<terms.size();++i) v=compute("core.compute.logicAnd",bit,{v,terms[i]});
            return v;
        }
        ValueId boundedAddress(ValueId address,uint64_t base,uint64_t count,ValueId &enable) {
            const auto width=type(address).width;
            if(base) {
                auto start=constant(typeId(address),base);
                auto lower=compute("core.compute.ge",bit,{address,start});
                enable=compute("core.compute.logicAnd",bit,{enable,lower});
                address=compute("core.compute.sub",typeId(address),{address,start});
            }
            // Compare in row space: base+count may be 2^64 even though
            // every original decoded address is representable.
            if(width>=64 || count<(uint64_t{1}<<width)) {
                auto end=constant(typeId(address),count);
                auto upper=compute("core.compute.lt",bit,{address,end});
                enable=compute("core.compute.logicAnd",bit,{enable,upper});
            }
            return address;
        }
        void rewriteGroup(Group &g, bool merge) {
            const auto element=m.states()[g.rows[0].index-1].type;
            const auto origin=m.states()[g.rows[0].index-1].origin;
            const auto name="__reg_to_mem_"+std::to_string(g.rows[0].index);
            auto table=m.addState(name,m.arrayType(element,g.rows.size()),origin);
            std::vector<InitStep> steps;std::vector<Parameter> parameters;
            for(std::size_t row=0;row<g.rows.size();++row) {
                const auto old=g.rows[row]; rowMap[old.index]={table,row}; removedStates[old.index]=1;
                const auto &init=m.steps(*inits[old.index])[0];
                auto value=*param<std::string>(m,m.parameters(init),"value");
                const auto offset=static_cast<uint32_t>(parameters.size());
                parameters.push_back({m.intern("value"),value});
                parameters.push_back({m.intern("start"),static_cast<int64_t>(row)});
                parameters.push_back({m.intern("count"),int64_t{1}});
                steps.push_back({m.intern("core.init.fill"),{offset,3}});
            }
            // addInit can relocate its backing arrays; saved InitRecord pointers
            // remain valid only until here. Rebind the index after each append.
            m.addInit(table,steps,parameters);
            inits.resize(m.states().size()+1);
            for(const auto &init:m.initRecords()) inits[init.state.index]=&init;
            if(!merge) return;
            const auto &first=g.writes.front();
            std::vector<ValueId> sequence;
            ValueId previousAddress;
            std::vector<ObjectRef> objects{ObjectRef::state(table)};
            for(auto h:first.histories) objects.push_back(ObjectRef::state(h));
            std::vector<Parameter> ps{{m.intern("event_edges"),first.edges}};
            std::map<std::string,std::pair<ValueId,ValueId>> fills;
            // Emit one triple per logical write source in low-to-high priority
            // order.  Each scalar row carries the same source after family
            // validation; iterating rows here would write different row data
            // to the same dynamic address and make the last row win.
            for (auto index : g.order) {
                if (index >= first.branches.size()) { sequence.clear(); break; }
                const auto &branch = first.branches[index];
                auto enable = conjunction(branch.guard.terms);
                if (m.types()[element.index - 1].width == 1 && !ones(first.mask))
                    enable = compute("core.compute.logicAnd", bit, {enable, first.mask});
                if (!branch.guard.address) {
                    const auto k = key(branch.data);
                    auto it = fills.find(k);
                    if (it == fills.end()) fills.emplace(k, std::pair{enable, branch.data});
                    else it->second.first = compute("core.compute.logicOr", bit, {it->second.first, enable});
                    continue;
                }
                auto address = boundedAddress(branch.guard.address, g.base, g.rows.size(), enable);
                if(options.enableSameAddressFusion && previousAddress==branch.guard.address && !sequence.empty()) {
                    const auto offset=sequence.size()-3;
                    sequence[offset+2]=compute("core.compute.mux",element,{enable,branch.data,sequence[offset+2]});
                    sequence[offset]=compute("core.compute.logicOr",bit,{sequence[offset],enable});
                } else sequence.insert(sequence.end(), {enable, address, branch.data});
                previousAddress=branch.guard.address;
            }
            if(sequence.size()==3) {
                sequence.push_back(first.mask); sequence.insert(sequence.end(),first.events.begin(),first.events.end());
                m.addOperation("core.state.memWrite",sequence,{},objects,ps);
            } else if(!sequence.empty()) {
                sequence.insert(sequence.end(),first.events.begin(),first.events.end());
                m.addOperation("core.state.memWriteSeq",sequence,{},objects,ps);
            }
            for(const auto &[key,fill]:fills) {
                (void)key;std::vector<ValueId> a{fill.first,fill.second};a.insert(a.end(),first.events.begin(),first.events.end());
                m.addOperation("core.state.memFill",a,{},objects,ps);
            }
            for(const auto &w:g.writes) {
                removedOps[w.op.index]=1;
                if(w.op!=first.op) for(auto h:w.histories) removedStates[h.index]=1;
            }
        }
        void rewriteAccesses(std::size_t originalOps) {
            std::unordered_map<uint32_t,ValueId> rowConstants;
            for(std::size_t i=0;i<originalOps;++i) {
                auto op=m.operations()[i]; if(removedOps[op.id.index]) continue;
                const auto objects=m.objectRefs(op);
                if(objects.empty() || objects[0].kind!=ObjectKind::State || !rowMap[objects[0].index].first) continue;
                const auto [table,row]=rowMap[objects[0].index];
                auto [it,inserted]=rowConstants.emplace(row,ValueId{});
                if(inserted) it->second=constant(indexType,row);
                const auto address=it->second;
                std::vector<ObjectRef> refs(objects.begin(),objects.end()); refs[0]=ObjectRef::state(table);
                std::vector<ValueId> results(m.results(op).begin(),m.results(op).end());
                std::vector<ValueId> args(m.operands(op).begin(),m.operands(op).end());
                std::vector<Parameter> ps(m.parameters(op).begin(),m.parameters(op).end());
                if(kind(op)=="core.state.read") {
                    m.replaceOperation(op.id,"core.state.memRead",std::array{address},results,refs,ps);
                } else {
                    args.insert(args.begin()+1,address);
                    m.replaceOperation(op.id,"core.state.memWrite",args,results,refs,ps);
                }
            }
        }
        void rewritePackedReads() {
            for(const auto &view:packedViews) {
                if(!view.selected) continue;
                const auto [table,base]=rowMap[view.lanes[0].index];
                if(!table) continue;
                const auto count=m.types()[m.states()[table.index-1].type.index-1].count;
                bool compatible=true,periodic=true;
                std::vector<uint32_t> lanes;
                for(std::size_t i=0;i<view.lanes.size();++i) {
                    const auto [owner,row]=rowMap[view.lanes[i].index];
                    if(owner!=table) {compatible=false;break;}
                    periodic &= row==(base+i)%count;
                    lanes.push_back(row);
                }
                if(!compatible) continue;
                const auto runs=laneRuns(lanes,count);
                if(runs.size()>8) continue;
                const auto element=m.types()[m.states()[table.index-1].type.index-1].elementType;
                const auto elementWidth=m.types()[element.index-1].width;
                for(const auto &slice:view.slices) {
                    const auto id=slice.op;
                    const auto op=m.operations()[id.index-1];
                    const auto output=m.results(op)[0];
                    auto address=slice.index;
                    // Widen before adding the base, and clamp before memRead.
                    // The outer mux restores the original zero on an invalid lane.
                    const auto addressType=m.logicType(64,false,LogicDomain::TwoState);
                    if(typeId(address)!=addressType)
                        address=compute("core.compute.assign",addressType,{address});
                    auto zeroAddress=constant(addressType,0);
                    const auto partType=slice.bitWindow?bit:element;
                    const auto zero=constant(partType,0);
                    std::vector<ValueId> parts;
                    const auto width=slice.bitWindow?type(output).width:1;
                    const uint64_t viewSize=view.lanes.size()*uint64_t{slice.bitWindow?elementWidth:1};
                    for(uint32_t i=width;i>0;--i) {
                        const uint64_t offset=slice.offset+i-1;
                        if(offset>=viewSize) {parts.push_back(zero);continue;}
                        // Compare before addition: UINT64_MAX must not wrap
                        // back into a valid row, and a partial window zero-pads.
                        auto inRange=compute("core.compute.lt",bit,{address,constant(addressType,viewSize-offset)});
                        auto safe=compute("core.compute.mux",addressType,{inRange,address,zeroAddress});
                        auto laneOffset=offset;
                        ValueId bitOffset;
                        if(slice.bitWindow && elementWidth>1) {
                            if(offset) safe=compute("core.compute.add",addressType,{safe,constant(addressType,offset)});
                            const auto stride=constant(addressType,elementWidth);
                            bitOffset=compute("core.compute.mod",addressType,{safe,stride});
                            safe=compute("core.compute.div",addressType,{safe,stride});
                            laneOffset=0;
                        }
                        if(periodic) {
                            if(base+laneOffset) safe=compute("core.compute.add",addressType,{safe,constant(addressType,base+laneOffset)});
                            if(base+view.lanes.size()>count)
                                safe=compute("core.compute.mod",addressType,{safe,constant(addressType,count)});
                        } else {
                            if(laneOffset) safe=compute("core.compute.add",addressType,{safe,constant(addressType,laneOffset)});
                            const auto lane=safe;
                            ValueId mapped;
                            for(auto run=runs.rbegin();run!=runs.rend();++run) {
                                auto row=constant(addressType,run->row);
                                if(!run->repeated && run->end-run->begin>1) {
                                    // Clamp the run-local index as well: compute nodes
                                    // may be evaluated even when their outer mux loses.
                                    auto lower=compute("core.compute.ge",bit,{lane,constant(addressType,run->begin)});
                                    auto local=compute("core.compute.mux",addressType,{lower,lane,constant(addressType,run->begin)});
                                    if(run->begin) local=compute("core.compute.sub",addressType,{local,constant(addressType,run->begin)});
                                    if(run->row) local=compute("core.compute.add",addressType,{local,row});
                                    row=compute("core.compute.mod",addressType,{local,constant(addressType,count)});
                                }
                                if(!mapped) mapped=row;
                                else {
                                    auto beforeEnd=compute("core.compute.lt",bit,{lane,constant(addressType,run->end)});
                                    mapped=compute("core.compute.mux",addressType,{beforeEnd,row,mapped});
                                }
                            }
                            safe=mapped;
                        }
                        const auto readKey=std::pair{table.index,safe.index};
                        auto [read,inserted]=createdReads.emplace(readKey,ValueId{});
                        if(inserted) {
                            read->second=m.addValue(element);
                            m.addOperation("core.state.memRead",std::array{safe},std::array{read->second},std::array{ObjectRef::state(table)});
                        }
                        auto selected=read->second;
                        if(bitOffset) selected=compute("core.compute.sliceDynamic",bit,{selected,bitOffset},
                            {{m.intern("sliceWidth"),int64_t{1}}});
                        else if(slice.bitWindow && element!=bit) selected=compute("core.compute.assign",bit,{selected});
                        parts.push_back(compute("core.compute.mux",partType,{inRange,selected,zero}));
                    }
                    m.replaceOperation(id,parts.size()==1?"core.compute.assign":"core.compute.concat",parts,std::array{output});
                }
            }
        }
        void cleanup() {
            removedOps.resize(m.operations().size()+1); removedStates.resize(m.states().size()+1);
            std::vector<uint32_t> uses(m.values().size()+1);
            std::vector<OpId> producers(m.values().size()+1);
            for(const auto &op:m.operations()) if(!removedOps[op.id.index]) {
                for(auto v:m.results(op)) producers[v.index]=op.id;
                for(auto v:m.operands(op)) ++uses[v.index];
            }
            const auto removable=[&](const SimOp &op) {
                const auto k=kind(op);
                if(!k.starts_with("core.compute.") && k!="core.state.read" && k!="core.state.memRead") return false;
                for(auto v:m.results(op)) if(uses[v.index]) return false;
                return true;
            };
            std::vector<OpId> pending;
            for(const auto &op:m.operations()) if(!removedOps[op.id.index] && removable(op)) pending.push_back(op.id);
            while(!pending.empty()) {
                auto id=pending.back();pending.pop_back();if(removedOps[id.index]) continue;
                const auto &op=m.operations()[id.index-1]; if(!removable(op)) continue;
                removedOps[id.index]=1;
                for(auto v:m.operands(op)) if(!--uses[v.index] && producers[v.index]) pending.push_back(producers[v.index]);
            }
            m.compact(removedOps,removedStates);
        }
        int64_t operationCost(const SimOp &op) const {
            uint64_t words=1;
            for(auto result:m.results(op)) words=std::max(words,(uint64_t{type(result).width}+63)/64);
            if(kind(op)=="core.compute.constant" || kind(op)=="core.compute.assign") return 0;
            if(kind(op)=="core.compute.concat") return words+static_cast<int64_t>(m.operands(op).size());
            if(kind(op)=="core.state.regWrite") return 2;
            return static_cast<int64_t>(words);
        }
        // Count only the old nodes whose last use is removed, stopping at inputs
        // needed by replacement operations. No per-candidate copy of the full DAG.
        int64_t removedCost(const std::vector<OpId> &roots,const std::set<uint32_t> &retained) const {
            std::unordered_map<uint32_t,uint32_t> removedUses;
            std::unordered_set<uint32_t> dead;
            std::vector<OpId> pending=roots;
            int64_t cost=0;
            while(!pending.empty()) {
                const auto id=pending.back();pending.pop_back();
                if(!dead.insert(id.index).second) continue;
                const auto &op=m.operations()[id.index-1];
                cost+=operationCost(op);
                for(auto value:m.operands(op)) {
                    if(++removedUses[value.index]!=valueUses[value.index] || retained.contains(value.index)) continue;
                    const auto *producer=def(value);
                    if(!producer || (!kind(*producer).starts_with("core.compute.") && kind(*producer)!="core.state.read")) continue;
                    bool unused=true;
                    for(auto result:m.results(*producer))
                        unused &= !retained.contains(result.index) && removedUses[result.index]==valueUses[result.index];
                    if(unused) pending.push_back(producer->id);
                }
            }
            return cost;
        }
        bool selectCostPlan(Group &g,bool canMerge) {
            std::unordered_map<uint32_t,uint32_t> rows;
            for(uint32_t i=0;i<g.rows.size();++i) rows.emplace(g.rows[i].index,i);
            std::vector<std::size_t> views;
            std::vector<OpId> readRoots,writeRoots;
            std::set<uint32_t> readRetained,writeRetained;
            std::set<std::string> readExpressions;
            int64_t readAdded=0,writeAdded=0,fixedReadPenalty=0;
            const auto element=m.types()[m.states()[g.rows[0].index-1].type.index-1];
            const auto keep=[&](ValueId v) {if(v) writeRetained.insert(v.index);};
            for(auto state:g.rows) for(auto id:refs[state.index])
                if(kind(m.operations()[id.index-1])=="core.state.read") ++fixedReadPenalty;
            if(options.enableReadRewrite) for(auto state:g.rows) {
                const auto found=viewsByFirstState.find(state.index);
                if(found==viewsByFirstState.end()) continue;
                for(auto index:found->second) {
                    const auto &view=packedViews[index];
                    std::vector<uint32_t> lanes;
                    for(auto lane:view.lanes) {
                        const auto row=rows.find(lane.index);
                        if(row==rows.end()) {lanes.clear();break;}
                        lanes.push_back(row->second);
                    }
                    if(lanes.empty()) continue;
                    const auto runs=laneRuns(lanes,g.rows.size());
                    if(runs.size()>8) continue;
                    views.push_back(index);
                    for(const auto &slice:view.slices) {
                        readRoots.push_back(slice.op);
                        readRetained.insert(slice.index.index);
                        const auto width=slice.bitWindow?type(m.results(m.operations()[slice.op.index-1])[0]).width:1;
                        for(uint32_t i=0;i<width;++i) {
                            // Identical source/index/offset requests share generated
                            // guards and reads. Run selection is charged per request.
                            const auto signature=std::to_string(view.packed.index)+"/"+std::to_string(slice.index.index)+"/"+
                                std::to_string(slice.offset+i)+"/"+std::to_string(slice.bitWindow);
                            if(readExpressions.insert(signature).second)
                                readAdded+=5+(runs.size()-1)*6+(slice.bitWindow && element.width>1?3:0);
                        }
                        if(width>1) ++readAdded;
                    }
                }
            }
            if(canMerge) {
                for(const auto &write:g.writes) writeRoots.push_back(write.op);
                const auto &first=g.writes[0];
                keep(first.mask);
                for(auto value:first.events) keep(value);
                for(const auto &branch:first.branches) {
                    keep(branch.guard.address);keep(branch.data);
                    for(auto value:branch.guard.terms) keep(value);
                    writeAdded+=static_cast<int64_t>(branch.guard.terms.size())+3;
                    if(branch.guard.address) writeAdded+=2+(g.base?3:0);
                    else writeAdded+=g.rows.size(); // Broadcast touches every cell.
                }
            }
            std::vector<OpId> combined=writeRoots;
            combined.insert(combined.end(),readRoots.begin(),readRoots.end());
            auto retained=writeRetained;
            retained.insert(readRetained.begin(),readRetained.end());
            g.writeSavings=canMerge?removedCost(writeRoots,writeRetained)-writeAdded-fixedReadPenalty:0;
            g.readSavings=views.empty()?0:removedCost(readRoots,readRetained)-readAdded-fixedReadPenalty;
            g.combinedSavings=canMerge && !views.empty()?
                removedCost(combined,retained)-writeAdded-readAdded-fixedReadPenalty:0;
            bool useReads=!views.empty(),useWrites=canMerge;
            if(options.enableCostSelection) {
                const auto best=std::max({int64_t{0},g.writeSavings,g.readSavings,g.combinedSavings});
                if(!best) return false;
                if(canMerge && !views.empty() && best==g.combinedSavings) {useWrites=true;useReads=true;}
                else if(canMerge && best==g.writeSavings) {useWrites=true;useReads=false;}
                else {useWrites=false;useReads=true;}
            }
            g.mergeWrites=useWrites;
            if(useReads) for(auto index:views) packedViews[index].selected=true;
            g.detail+=(g.detail.empty()?"":"; ")+std::string("plan=")+
                (useWrites?(useReads?"writes+reads":"writes"):"reads")+"; cost units are estimates; activity unknown";
            return useWrites || useReads;
        }
        void prepare() {
            std::vector<uint8_t> claimed(m.states().size()+1);
            // Larger compatible storage families win deterministically.
            std::stable_sort(groups.begin(),groups.end(),[](const Group &a,const Group &b){
                if(a.rows.size()!=b.rows.size()) return a.rows.size()>b.rows.size();
                return a.writeSource>b.writeSource;
            });
            for(auto &g:groups) {
                g.selected=false;
                bool overlap=false;for(auto s:g.rows) overlap|=claimed[s.index]!=0;
                if(overlap) {g.reason="ownership";continue;}
                const bool merge=planWrites(g);
                if(!merge && !(options.enableReadRewrite && g.indexedRead)) {
                    if(g.reason.empty()) g.reason="write-shape";continue;
                }
                if(!selectCostPlan(g,merge)) {g.reason="cost";continue;}
                g.selected=true;
                g.reason=g.mergeWrites?"eligible":"eligible-indexed-read";
                for(auto s:g.rows) claimed[s.index]=1;
            }
        }
        std::size_t apply() {
            const auto oldOps=m.operations().size();
            removedOps.resize(oldOps+1);removedStates.resize(m.states().size()+1);rowMap.resize(m.states().size()+1);
            std::size_t count=0;
            for(auto &g:groups) {
                if(!g.selected) continue;
                if(!count) {
                    mutationStarted=true;
                    bit=m.logicType(1,false,LogicDomain::TwoState);
                    indexType=m.logicType(32,false,LogicDomain::TwoState);
                }
                rewriteGroup(g,g.mergeWrites);g.reason=g.mergeWrites?"merged":"indexed-read";++count;
            }
            if(count) {
                rewriteAccesses(oldOps);
                if(options.enableReadRewrite) rewritePackedReads();
                cleanup();
            }
            return count;
        }

        GrhSimModel &m;
        const RegToMemOptions &options;
        std::vector<OpId> defs;
        std::vector<uint32_t> valueUses;
        std::vector<ValueId> canonical;
        std::vector<std::vector<OpId>> refs,writers;
        std::vector<int8_t> eligibility;
        std::map<std::string,std::pair<std::size_t,std::string>> excludedStates;
        std::vector<const InitRecord*> inits;
        std::vector<Group> groups;
        std::vector<PackedView> packedViews;
        std::unordered_map<uint32_t,std::vector<std::size_t>> viewsByFirstState;
        std::unordered_map<std::string,ValueId> createdComputes;
        std::map<std::pair<uint32_t,uint32_t>,ValueId> createdReads;
        std::string parseReason;
        bool mutationStarted=false;
        std::unordered_set<uint32_t> decodedAddresses;
        TypeId bit,indexType;
        std::vector<uint8_t> removedOps,removedStates;
        std::vector<std::pair<StateId,uint32_t>> rowMap;
    };
}

RegToMemPass::RegToMemPass(RegToMemOptions options)
    : Pass("grhsim.reg-to-mem", options.analysisOnly?PassKind::Analysis:PassKind::SemanticTransform), options_(std::move(options)) {}

PassResult RegToMemPass::run(GrhSimModel &model, diag::Diagnostics &diagnostics) {
    const auto oldStates=model.states().size(),oldOps=model.operations().size();
    Engine engine(model,options_); engine.discover();
    // Cache report names before dense state IDs are rebuilt.
    std::unordered_map<uint32_t,std::string> names;
    for(const auto &g:engine.groups) names[g.rows[0].index]=model.text(model.states()[g.rows[0].index-1].name);
    std::size_t changed=0;
    try {
        engine.prepare();
        if(!options_.analysisOnly) changed=engine.apply();
    } catch(const std::exception &error) {
        if(engine.mutationStarted) model.poison();
        diagnostics.error(error.what());
        return {false,engine.mutationStarted,{}};
    }
    std::ostringstream report;
    report<<"source\trows\tbase\twrites\tstatus\tfirst_state\tdetail\twrite_savings\tread_savings\tcombined_savings\n";
    for(const auto &g:engine.groups) report<<(g.writeSource?"write":"read")<<'\t'<<g.rows.size()<<'\t'<<g.base<<'\t'
        <<(g.writes.empty()?0:g.writes[0].branches.size())<<'\t'<<g.reason<<'\t'<<names[g.rows[0].index]<<'\t'<<g.detail
        <<'\t'<<g.writeSavings<<'\t'<<g.readSavings<<'\t'<<g.combinedSavings<<'\n';
    for(const auto &[reason,entry]:engine.excludedStates)
        report<<"excluded-state\t"<<entry.first<<"\t0\t0\t"<<reason<<'\t'<<entry.second
            <<"\tstate exclusions before candidate discovery; rows is a state count\t0\t0\t0\n";
    if(!options_.report.empty()) {
        std::ofstream out(options_.report); if(!out) {diagnostics.error("cannot open reg-to-mem report");return {false,changed!=0,{}};}
        out<<report.str(); if(!out) {diagnostics.error("cannot write reg-to-mem report");return {false,changed!=0,{}};}
    }
    diagnostics.info("reg-to-mem candidates="+std::to_string(engine.groups.size())+" transformed="+std::to_string(changed)+
        " states="+std::to_string(oldStates)+"->"+std::to_string(model.states().size())+
        " operations="+std::to_string(oldOps)+"->"+std::to_string(model.operations().size()));
    return {true,changed!=0,{}};
}

void registerRegToMemPass(PassRegistry &registry) {
    std::string error;
    // Analysis uses a distinct registry entry so PassKind remains truthful.
    for(bool analysis:{false,true}) registry.registerPass(analysis?"grhsim.reg-to-mem-analyze":"grhsim.reg-to-mem",
        analysis?PassKind::Analysis:PassKind::SemanticTransform,
        [analysis](std::span<const std::string_view> args,std::string &error)->std::unique_ptr<Pass> {
            RegToMemOptions o; o.analysisOnly=analysis;
            for(std::size_t i=0;i<args.size();i+=2) {
                if(i+1==args.size()) {error="reg-to-mem option requires a value";return {};}
                const auto name=args[i], value=args[i+1];
                if(name=="--report") o.report=value;
                else if(name=="--min-element-count") {
                    auto [end,ec]=std::from_chars(value.data(),value.data()+value.size(),o.minElementCount);
                    if(ec!=std::errc{} || end!=value.data()+value.size() || o.minElementCount<2) {error="invalid min-element-count";return {};}
                } else {
                    bool *flag=nullptr;
                    if(name=="--enable-read-rewrite") flag=&o.enableReadRewrite;
                    if(name=="--enable-write-merge") flag=&o.enableWriteMerge;
                    if(name=="--enable-same-address-fusion") flag=&o.enableSameAddressFusion;
                    if(name=="--enable-cost-selection") flag=&o.enableCostSelection;
                    if(!flag || (value!="true" && value!="false")) {error="unknown reg-to-mem option or invalid boolean";return {};}
                    *flag=value=="true";
                }
            }
            return std::make_unique<RegToMemPass>(std::move(o));
        },error);
}
}
