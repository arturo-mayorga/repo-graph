// Cycles: the one structural fact an architecture view must never swallow.
//
// The radial layout spans a tree breadth first, which turns every back edge into a
// cross-link it ignores -- fine as a layout, fatal as a reading, because a cycle is the
// thing a reviewer most needs to be told about and the thing the arrangement is least
// able to show. So it is computed rather than inferred from the picture.

#include "TestMain.h"

#include "rgv/analysis/Cycles.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace rgv;

namespace {

using E = std::pair<NodeId, NodeId>;

// The members of the group containing `id`, or empty when it is in none.
std::vector<NodeId> group_with(const std::vector<analysis::CycleGroup>& gs, const NodeId& id) {
    for (const auto& g : gs) {
        if (std::find(g.nodes.begin(), g.nodes.end(), id) != g.nodes.end()) return g.nodes;
    }
    return {};
}

} // namespace

TEST(a_layered_graph_has_no_cycles) {
    // a -> b -> c, the shape a healthy architecture has.
    const auto gs = analysis::find_cycles({E{"a", "b"}, E{"b", "c"}});
    CHECK_EQ(gs.size(), 0u);
}

TEST(two_modules_that_import_each_other_are_one_cycle) {
    const auto gs = analysis::find_cycles({E{"a", "b"}, E{"b", "a"}});
    CHECK_EQ(gs.size(), 1u);
    CHECK_EQ(gs[0].nodes.size(), 2u);
    CHECK_EQ(gs[0].nodes[0], std::string("a"));
    CHECK_EQ(gs[0].nodes[1], std::string("b"));
}

// The whole ring is the finding, not the one edge that happens to close it: which edge
// "closes" a ring depends on where you started walking, and naming one of three as the
// culprit would be an arbitrary accusation.
TEST(a_longer_ring_names_every_module_in_it) {
    const auto gs = analysis::find_cycles({E{"a", "b"}, E{"b", "c"}, E{"c", "a"}});
    CHECK_EQ(gs.size(), 1u);
    CHECK_EQ(gs[0].nodes.size(), 3u);
}

TEST(a_module_hanging_off_a_ring_is_not_in_it) {
    // d imports into the ring and nothing imports d.
    const auto gs = analysis::find_cycles({E{"a", "b"}, E{"b", "a"}, E{"d", "a"}});
    CHECK_EQ(gs.size(), 1u);
    CHECK_EQ(gs[0].nodes.size(), 2u);
    CHECK_EQ(group_with(gs, "d").size(), 0u);
}

TEST(two_separate_rings_are_two_findings) {
    const auto gs = analysis::find_cycles({E{"a", "b"}, E{"b", "a"}, E{"y", "z"}, E{"z", "y"}});
    CHECK_EQ(gs.size(), 2u);
    CHECK_EQ(group_with(gs, "a").size(), 2u);
    CHECK_EQ(group_with(gs, "z").size(), 2u);
}

// A module that imports itself is a parse artefact, not an architecture problem, and
// the view drops such an edge long before this. Reporting it would be noise.
TEST(a_self_import_is_not_a_cycle) {
    const auto gs = analysis::find_cycles({E{"a", "a"}});
    CHECK_EQ(gs.size(), 0u);
}

// The report is read by a person and diffed between frames, so it must not reorder
// itself because the edges arrived in a different order.
TEST(the_report_does_not_depend_on_the_order_the_edges_arrive_in) {
    const std::vector<E> one{E{"b", "a"}, E{"a", "b"}, E{"z", "y"}, E{"y", "z"}};
    const std::vector<E> two{E{"y", "z"}, E{"a", "b"}, E{"z", "y"}, E{"b", "a"}};
    const auto           ga = analysis::find_cycles(one);
    const auto           gb = analysis::find_cycles(two);
    CHECK_EQ(ga.size(), gb.size());
    for (std::size_t i = 0; i < ga.size(); ++i) CHECK(ga[i].nodes == gb[i].nodes);
}

// Deep graphs are ordinary at file level -- a long import chain is a few thousand
// nodes -- so the walk must not be a recursion that runs out of stack.
TEST(a_very_deep_chain_does_not_overflow_the_stack) {
    std::vector<E> edges;
    for (int i = 0; i < 50000; ++i) {
        edges.push_back(E{"n" + std::to_string(i), "n" + std::to_string(i + 1)});
    }
    const auto gs = analysis::find_cycles(edges);
    CHECK_EQ(gs.size(), 0u);

    // And the same chain with its tail tied back to its head is exactly one finding.
    edges.push_back(E{"n50000", "n0"});
    const auto tied = analysis::find_cycles(edges);
    CHECK_EQ(tied.size(), 1u);
    CHECK_EQ(tied[0].nodes.size(), 50001u);
}
