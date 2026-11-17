#include "../../lib/transform/activity_shared_refinement.hpp"
#include <iostream>
#include <stdexcept>
using namespace wolvrix::lib::transform::detail;
using Groups = std::vector<std::vector<uint32_t>>;
static void require(bool pass, const char *msg) { if (!pass) throw std::runtime_error(msg); }
static Groups members(const SharedRefinement &r) {
    Groups result;
    for (const auto &p : r.partitions) { auto n=p.nodes; std::sort(n.begin(),n.end()); result.push_back(n); }
    std::sort(result.begin(),result.end());return result;
}
int main() {
 try {
    // Equal endpoint chain contraction is safe. Mixed unrelated original group
    // must stay intact, even though it has two different terminal classes.
    Groups dag{{1},{2},{},{},{}};
    Groups original{{0},{1},{2},{3,4}}, terminal{{1,2},{1,2},{1},{7},{8}};
    auto r=refineSharedPartitions(dag,original,terminal,{{0,1}});
    require(r.rounds==0 && r.splitGroups==0 && r.rejected==0,"acyclic candidate was refined");
    require(members(r)==Groups{{0,1},{2},{3,4}},"unrelated mixed group split");
    // Original order {0,1} -> {2}. Extract 0 and 2 into S, leaving
    // S -> {1} -> S. Synthetic terminal classes permit no split of {1};
    // reject this class while preserving an independent useful contraction.
    dag={{1},{2},{},{4},{},{}};
    original={{0,1},{2},{3},{4},{5}};
    terminal={{1,2},{1,2},{1,2},{7,8},{7,8},{9}};
    r=refineSharedPartitions(dag,original,terminal,{{0,2},{3,4}});
    require(r.rejected==1 && !r.accepted[0] && r.accepted[1],"fallback rejected unrelated class");
    require(members(r)==Groups{{0,1},{2},{3,4},{5}},"fallback did not restore original remainder");
    // Acyclic node graph: 0->1, 2->3. Groups {0,2}, {1,3} were ordered.
    // Merge {0,3}: S->{1,2}->S. Only mixed remainder {1,2} needs splitting.
    // Its acyclic descendant {4,5} must not be included (unlike Kahn leftovers).
    dag={{1},{4},{3},{4},{},{}};
    original={{0,2},{1,3},{4,5}};
    terminal={{1,2},{1},{1,2},{1,2},{6},{7}};
    r=refineSharedPartitions(dag,original,terminal,{{0,3}});
    // Here extraction leaves separate singletons already: no SCC.
    require(r.rounds==0,"separate remainders unexpectedly cyclic");
    original={{0},{1,2},{3},{4,5}};
    r=refineSharedPartitions(dag,original,terminal,{{0,3}});
    require(r.rounds==1 && r.splitGroups==1 && r.rejected==0,"cycle remainder not refined");
    require(members(r)==Groups{{0,3},{1},{2},{4,5}},"acyclic descendant split");
    const auto cyclic=cyclicSharedPartitions(dag,r.partitions);
    require(std::none_of(cyclic.begin(),cyclic.end(),[](bool b){return b;}),"quotient still cyclic");
    // Deep DAG checks the nonrecursive SCC walk; same partition is a fixed point.
    const unsigned n=50000;dag.assign(n,{});original.clear();terminal.assign(n,{1});
    for(unsigned i=0;i<n;++i) {if(i+1<n)dag[i].push_back(i+1);original.push_back({i});}
    r=refineSharedPartitions(dag,original,terminal,{});
    require(r.partitions.size()==n && r.rounds==0,"deep DAG changed");
    // Independent Kahn check across deterministic acyclic node graphs with
    // arbitrary contractions, including mixtures requiring rejection/refinement.
    uint32_t rng=12345;
    auto next=[&](){rng=rng*1664525u+1013904223u;return rng;};
    for(unsigned trial=0;trial<100;++trial) {
        constexpr unsigned count=24;
        dag.assign(count,{});original.assign(8,{});terminal.assign(count,{});
        Groups classes(3);
        for(unsigned i=0;i<count;++i) {
            original[i/3].push_back(i);terminal[i]={next()%4};
            for(unsigned j=i+1;j<count;++j)if(next()%11==0)dag[i].push_back(j);
            const auto c=next()%5;if(c<3)classes[c].push_back(i);
        }
        r=refineSharedPartitions(dag,original,terminal,classes);
        std::vector<unsigned> owner(count,count),seen(count,0),degree(r.partitions.size(),0);
        Groups quotient(r.partitions.size());
        for(unsigned g=0;g<r.partitions.size();++g)for(auto node:r.partitions[g].nodes) {
            require(node<count && ++seen[node]==1,"random node duplicated");owner[node]=g;
        }
        for(auto n:seen)require(n==1,"random node lost");
        for(unsigned i=0;i<count;++i)for(auto j:dag[i])if(owner[i]!=owner[j]) {
            quotient[owner[i]].push_back(owner[j]);++degree[owner[j]];
        }
        std::vector<unsigned> ready;for(unsigned i=0;i<degree.size();++i)if(!degree[i])ready.push_back(i);
        unsigned visited=0;
        while(!ready.empty()){auto g=ready.back();ready.pop_back();++visited;for(auto h:quotient[g])if(--degree[h]==0)ready.push_back(h);}
        require(visited==degree.size(),"random quotient remains cyclic");
    }
    std::cout<<"shared quotient cycle refinement passed\n";
 } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
