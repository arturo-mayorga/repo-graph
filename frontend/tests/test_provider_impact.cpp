// The provider's blast radius. What the frontend receives as `impact.updated` when a
// real checkout is watched, so what it gets wrong the user sees as a missing or a
// spurious highlight with nothing to argue against.

#include "TestMain.h"

#include "Impact.h"

#include <string>
#include <vector>

using namespace rgv;

namespace {

watch::GraphEdge edge(const std::string& from, const std::string& to, long valid_from = 100,
                      bool heuristic = false) {
    return watch::GraphEdge{"e:" + from + "->" + to, from, to, valid_from, heuristic};
}

const watch::ImpactHit* hit(const std::vector<watch::ImpactHit>& hs, const std::string& n) {
    for (const auto& h : hs) {
        if (h.node == n) return &h;
    }
    return nullptr;
}

} // namespace

// a imports b imports c. Change b: a is impacted, c is not -- c does not depend on b.
TEST(impact_follows_edges_in_reverse_from_the_seed) {
    const std::vector<watch::GraphEdge> es{edge("a", "b"), edge("b", "c")};
    const auto hs = watch::reverse_reach(es, {"b"}, 100, 8);
    CHECK_EQ(hs.size(), 2u);
    const auto* a = hit(hs, "a");
    CHECK(a != nullptr);
    CHECK_EQ(a->distance, 1);
    CHECK_EQ(a->path.size(), 1u);
    CHECK_EQ(a->path[0], std::string("e:a->b"));
    const auto* b = hit(hs, "b");
    CHECK(b != nullptr);
    CHECK_EQ(b->distance, 0);
    CHECK(b->path.empty());
    CHECK(hit(hs, "c") == nullptr);
}

TEST(the_recorded_path_is_a_shortest_one) {
    // d -> a -> b -> s and d -> s: d is at distance 1, not 3.
    const std::vector<watch::GraphEdge> es{edge("d", "a"), edge("a", "b"), edge("b", "s"),
                                           edge("d", "s")};
    const auto  hs = watch::reverse_reach(es, {"s"}, 100, 8);
    const auto* d  = hit(hs, "d");
    CHECK(d != nullptr);
    CHECK_EQ(d->distance, 1);
    CHECK_EQ(d->path.size(), 1u);
    CHECK_EQ(d->path[0], std::string("e:d->s"));
    const auto* a = hit(hs, "a");
    CHECK_EQ(a->distance, 2);
    CHECK_EQ(a->path[0], std::string("e:a->b"));
    CHECK_EQ(a->path[1], std::string("e:b->s"));
}

TEST(traversal_stops_at_max_depth) {
    const std::vector<watch::GraphEdge> es{edge("a", "b"), edge("b", "c"), edge("c", "s")};
    const auto hs = watch::reverse_reach(es, {"s"}, 100, 2);
    CHECK(hit(hs, "b") != nullptr);
    CHECK(hit(hs, "a") == nullptr);
}

// Contract §2.4: heuristic edges are excluded from traversal by default.
TEST(heuristic_edges_do_not_carry_impact) {
    const std::vector<watch::GraphEdge> es{edge("a", "s", 100, true)};
    const auto hs = watch::reverse_reach(es, {"s"}, 100, 8);
    CHECK_EQ(hs.size(), 1u);
    CHECK(hit(hs, "a") == nullptr);
}

// FR-29: a node that is impacted only because an edge appeared after the baseline is a
// different event from one whose dependency's implementation changed.
TEST(impact_through_a_new_edge_is_attributed_to_the_dependency) {
    const std::vector<watch::GraphEdge> es{edge("a", "s", 100), edge("n", "s", 103)};
    const auto hs = watch::reverse_reach(es, {"s"}, 100, 8);
    CHECK(!hit(hs, "a")->dependency_added);
    CHECK(hit(hs, "n")->dependency_added);
}

TEST(a_cycle_terminates_and_seeds_stay_at_distance_zero) {
    const std::vector<watch::GraphEdge> es{edge("a", "b"), edge("b", "a"), edge("s", "a")};
    const auto hs = watch::reverse_reach(es, {"a", "s"}, 100, 8);
    CHECK_EQ(hit(hs, "a")->distance, 0);
    CHECK_EQ(hit(hs, "s")->distance, 0);
    CHECK_EQ(hit(hs, "b")->distance, 1);
}

TEST(results_are_sorted_by_node_id) {
    const std::vector<watch::GraphEdge> es{edge("z", "s"), edge("m", "s"), edge("a", "s")};
    const auto hs = watch::reverse_reach(es, {"s"}, 100, 8);
    CHECK_EQ(hs.size(), 4u);
    CHECK_EQ(hs[0].node, std::string("a"));
    CHECK_EQ(hs[1].node, std::string("m"));
    CHECK_EQ(hs[2].node, std::string("s"));
    CHECK_EQ(hs[3].node, std::string("z"));
}
