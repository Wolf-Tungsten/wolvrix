#include "../../lib/transform/activity_shared_components.hpp"
#include <iostream>

using namespace wolvrix::lib::transform::detail;
using Groups = std::vector<std::vector<uint32_t>>;
static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        Groups dag{{1}, {}, {3}, {}}, edges(4);
        std::vector<uint32_t> owners{0, 0, 1, 1};
        std::vector<SharedComponentNode> nodes(4);
        nodes[0].states = {100}; nodes[2].states = {200};
        nodes[1].work = nodes[3].work = true;
        nodes[1].inputs = {{0, 10}}; nodes[3].inputs = {{2, 20}};
        auto r = connectedSharedClasses(dag, edges, owners, {{0, 1, 2, 3}}, nodes);
        require(r.classes.empty(), "equal terminals pooled independent state computations");
        // Identical complete register activation, although distinct read SSA.
        nodes[2].states = {100};
        r = connectedSharedClasses(dag, edges, owners, {{0, 1, 2, 3}}, nodes);
        require(r.classes == Groups{{0, 1, 2, 3}} && r.equalActivationJoins == 1,
                "equal register activation failed to coalesce");
        nodes[3].inputs.push_back({noSharedGroup, 99});
        r = connectedSharedClasses(dag, edges, owners, {{0, 1, 2, 3}}, nodes);
        require(r.classes.empty(), "additional input omitted from activation proof");
        nodes[3].inputs.pop_back(); nodes[3].activationKnown = false;
        r = connectedSharedClasses(dag, edges, owners, {{0, 1, 2, 3}}, nodes);
        require(r.classes.empty(), "unknown activation accepted");
        nodes[3].activationKnown = true; nodes[2].states = {200};
        dag[1] = {3}; edges[1] = {3}; nodes[3].inputs.push_back({1, 30});
        r = connectedSharedClasses(dag, edges, owners, {{0, 1, 2, 3}}, nodes);
        require(r.classes == Groups{{0, 1, 2, 3}} && r.equalActivationJoins == 0,
                "true cross-owner computation was not contracted");
        r = connectedSharedClasses(dag, edges, {0, 0, 0, 0}, {{0, 1, 2, 3}}, nodes);
        require(r.classes.empty(), "already-local computation counted as a contraction");
        r = connectedSharedClasses(dag, edges, owners, {{0, 1}, {2, 3}}, nodes);
        require(r.classes.empty(), "different terminal sets merged");
        // A common read is not itself a computational connection. Only an
        // exact complete incoming value frontier permits the independent users.
        dag = {{1, 2}, {}, {}}; edges.assign(3, {}); owners = {0, 1, 2};
        nodes.assign(3, {}); nodes[0].states = {100};
        nodes[1].work = nodes[2].work = true;
        nodes[1].inputs = {{0, 10}}; nodes[2].inputs = {{0, 10}, {noSharedGroup, 11}};
        r = connectedSharedClasses(dag, edges, owners, {{0, 1, 2}}, nodes);
        require(r.classes.empty(), "common register bridged unequal activation components");
        nodes[2].inputs.pop_back();
        r = connectedSharedClasses(dag, edges, owners, {{0, 1, 2}}, nodes);
        require(r.classes == Groups{{0, 1, 2}}, "shared read did not follow equivalent users");
        // Leaving an unrelated mixed original group intact is delegated to the
        // independently tested SCC refinement, including exact fallback.
        dag.push_back({}); dag.push_back({});
        auto refined = refineSharedPartitions(dag, {{0}, {1}, {2}, {3, 4}},
                                              {{1, 2}, {1, 2}, {1, 2}, {3}, {4}}, r.classes);
        require(refined.splitGroups == 0 && refined.rejected == 0, "unrelated remainder split");
        require(std::any_of(refined.partitions.begin(), refined.partitions.end(), [](const auto &p) {
            return p.nodes == std::vector<uint32_t>{3, 4};
        }), "unrelated original membership lost");
        std::cout << "shared connected components passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
