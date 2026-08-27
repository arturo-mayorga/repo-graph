// Contract behaviours that are easy to get wrong and expensive to get wrong.
#include "TestMain.h"

#include "rgv/fixture/Json.h"
#include "rgv/model/GraphStore.h"
#include "rgv/sim/ImpactSim.h"

#include <algorithm>

using namespace rgv;

namespace {

Node mk_node(std::string id, NodeKind k, std::string parent = {}) {
    Node n;
    n.id     = std::move(id);
    n.kind   = k;
    n.name   = n.id;
    n.parent = std::move(parent);
    return n;
}

Edge mk_edge(std::string id, EdgeKind k, std::string from, std::string to) {
    Edge e;
    e.id   = std::move(id);
    e.kind = k;
    e.from = std::move(from);
    e.to   = std::move(to);
    return e;
}

// repo -> {pkg:a, pkg:b, pkg:c}, b depends on a, c depends on b.
Snapshot chain_snapshot() {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:a", NodeKind::Package, "repo"),
               mk_node("pkg:b", NodeKind::Package, "repo"),
               mk_node("pkg:c", NodeKind::Package, "repo"),
               mk_node("dir:a/src", NodeKind::Directory, "pkg:a"),
               mk_node("file:a/src/x.ts", NodeKind::File, "dir:a/src")};
    s.edges = {mk_edge("e:b->a", EdgeKind::DependsOn, "pkg:b", "pkg:a"),
               mk_edge("e:c->b", EdgeKind::DependsOn, "pkg:c", "pkg:b")};
    return s;
}

Event graph_event(Generation gen, GraphUpdatedPayload p) {
    Event e;
    e.type       = EventType::GraphUpdated;
    e.generation = gen;
    e.payload    = std::move(p);
    return e;
}

} // namespace

// A parse failure must not look like a deleted dependency (FR-19 / NFR-04). The store
// has to keep a stale edge addressable while a removed one is genuinely gone.
TEST(stale_edge_survives_while_removed_edge_does_not) {
    GraphStore store;
    store.reset(chain_snapshot());

    GraphUpdatedPayload p;
    Edge stale  = mk_edge("e:b->a", EdgeKind::DependsOn, "pkg:b", "pkg:a");
    stale.freshness = Freshness::Stale;
    p.updated_edges = {stale};
    p.removed_edges = {"e:c->b"};
    store.on_event(graph_event(101, p));

    CHECK(store.edge("e:b->a") != nullptr);
    CHECK(store.edge("e:b->a")->freshness == Freshness::Stale);
    CHECK(store.edge("e:c->b") == nullptr);
    // The stale edge must still be reachable by traversal, or the UI would understate
    // the blast radius exactly when it is least certain.
    CHECK_EQ(store.in_edges("pkg:a").size(), std::size_t{1});
}

// Deleting a node must take its incident edges with it, or traversal walks into
// endpoints that no longer exist.
TEST(erasing_a_node_cascades_to_incident_edges) {
    GraphStore store;
    store.reset(chain_snapshot());
    CHECK_EQ(store.edges().size(), std::size_t{2});

    GraphUpdatedPayload p;
    p.removed_nodes = {"pkg:b"};
    store.on_event(graph_event(101, p));

    CHECK(store.node("pkg:b") == nullptr);
    CHECK_EQ(store.edges().size(), std::size_t{0});
    CHECK_EQ(store.in_edges("pkg:a").size(), std::size_t{0});
}

// A file moving between packages is a real agent action; the old parent must not
// keep a phantom child.
TEST(reparenting_a_node_updates_both_child_lists) {
    GraphStore store;
    store.reset(chain_snapshot());
    CHECK_EQ(store.children("dir:a/src").size(), std::size_t{1});

    GraphUpdatedPayload p;
    p.updated_nodes = {mk_node("file:a/src/x.ts", NodeKind::File, "pkg:b")};
    store.on_event(graph_event(101, p));

    CHECK_EQ(store.children("dir:a/src").size(), std::size_t{0});
    CHECK_EQ(store.children("pkg:b").size(), std::size_t{1});
}

// FR-11: a changed file has to name its owning package without the caller knowing
// how deep the directory nesting goes.
TEST(ancestor_of_kind_projects_a_file_to_its_package) {
    GraphStore store;
    store.reset(chain_snapshot());
    CHECK_EQ(store.ancestor_of_kind("file:a/src/x.ts", NodeKind::Package), std::string("pkg:a"));
    CHECK_EQ(store.ancestor_of_kind("file:a/src/x.ts", NodeKind::Repository), std::string("repo"));
    CHECK_EQ(store.ancestor_of_kind("file:a/src/x.ts", NodeKind::Symbol), std::string(""));
}

// A malformed fixture must not hang the frame.
TEST(ancestor_walk_terminates_on_a_parent_cycle) {
    Snapshot s;
    s.nodes = {mk_node("x", NodeKind::Directory, "y"), mk_node("y", NodeKind::Directory, "x")};
    GraphStore store;
    store.reset(s);
    CHECK_EQ(store.ancestor_of_kind("x", NodeKind::Package), std::string(""));
}

TEST(reverse_bfs_gives_minimum_distance_and_a_path_that_reaches_the_seed) {
    GraphStore store;
    store.reset(chain_snapshot());

    ImpactFilters f;
    auto r = sim::compute(store, {"pkg:a"}, Level::Package, f);

    CHECK_EQ(r.impacted_nodes.size(), std::size_t{3});
    CHECK_EQ(r.impacted_nodes[0].node_id, std::string("pkg:a"));
    CHECK_EQ(r.impacted_nodes[0].min_distance, 0);
    CHECK(r.impacted_nodes[0].changed);
    CHECK_EQ(r.impacted_nodes[1].node_id, std::string("pkg:b"));
    CHECK_EQ(r.impacted_nodes[1].min_distance, 1);
    CHECK(r.impacted_nodes[1].direct);
    CHECK_EQ(r.impacted_nodes[2].node_id, std::string("pkg:c"));
    CHECK_EQ(r.impacted_nodes[2].min_distance, 2);
    CHECK(!r.impacted_nodes[2].direct);

    // Paths run impacted-node-first and each hop must chain: c --e:c->b--> b --e:b->a--> a
    const auto& path = r.impacted_nodes[2].paths.at(0).edges;
    CHECK_EQ(path.size(), std::size_t{2});
    CHECK_EQ(path[0], std::string("e:c->b"));
    CHECK_EQ(path[1], std::string("e:b->a"));
}

// Spec 10.2: containment is hierarchy, not dependency. Traversing it would make every
// file in a package look impacted by every other.
TEST(structural_edges_are_never_traversed_as_impact) {
    Snapshot s = chain_snapshot();
    s.edges.push_back(mk_edge("e:contains", EdgeKind::Contains, "pkg:c", "pkg:a"));
    GraphStore store;
    store.reset(s);

    ImpactFilters f;
    f.edge_kinds = {EdgeKind::DependsOn, EdgeKind::Contains};
    auto r       = sim::compute(store, {"pkg:a"}, Level::Package, f);

    auto it = std::find_if(r.impacted_nodes.begin(), r.impacted_nodes.end(),
                           [](const ImpactedNode& n) { return n.node_id == "pkg:c"; });
    CHECK(it != r.impacted_nodes.end());
    CHECK_EQ(it->min_distance, 2);   // reached via b, not via the contains shortcut
}

TEST(heuristic_edges_are_excluded_unless_explicitly_included) {
    Snapshot s   = chain_snapshot();
    s.edges[1].confidence = Confidence::Heuristic;   // c -> b
    GraphStore store;
    store.reset(s);

    ImpactFilters strict;
    CHECK_EQ(sim::compute(store, {"pkg:a"}, Level::Package, strict).impacted_nodes.size(),
             std::size_t{2});

    ImpactFilters loose;
    loose.include_heuristic = true;
    CHECK_EQ(sim::compute(store, {"pkg:a"}, Level::Package, loose).impacted_nodes.size(),
             std::size_t{3});
}

// An invalid provider result is not evidence; a stale one is. Dropping stale edges
// would shrink the blast radius exactly when confidence is lowest.
TEST(invalid_edges_drop_out_of_traversal_but_stale_edges_do_not) {
    {
        Snapshot s = chain_snapshot();
        s.edges[0].freshness = Freshness::Stale;   // b -> a
        GraphStore store;
        store.reset(s);
        CHECK_EQ(sim::compute(store, {"pkg:a"}, Level::Package, {}).impacted_nodes.size(),
                 std::size_t{3});
    }
    {
        Snapshot s = chain_snapshot();
        s.edges[0].freshness = Freshness::Invalid;
        GraphStore store;
        store.reset(s);
        CHECK_EQ(sim::compute(store, {"pkg:a"}, Level::Package, {}).impacted_nodes.size(),
                 std::size_t{1});
    }
}

// If any hop is stale the conclusion is stale. Reporting it as current would present
// stale evidence as current, which NFR-04 forbids outright.
TEST(a_stale_hop_makes_the_whole_impact_conclusion_stale) {
    Snapshot s = chain_snapshot();
    s.edges[0].freshness = Freshness::Stale;   // b -> a
    GraphStore store;
    store.reset(s);

    auto r = sim::compute(store, {"pkg:a"}, Level::Package, {});
    auto find = [&](const std::string& id) {
        return *std::find_if(r.impacted_nodes.begin(), r.impacted_nodes.end(),
                             [&](const ImpactedNode& n) { return n.node_id == id; });
    };
    CHECK(find("pkg:b").freshness == Freshness::Stale);
    CHECK(find("pkg:c").freshness == Freshness::Stale);   // inherited through the path
}

TEST(max_depth_bounds_the_traversal) {
    GraphStore store;
    store.reset(chain_snapshot());
    ImpactFilters f;
    f.max_depth = 1;
    CHECK_EQ(sim::compute(store, {"pkg:a"}, Level::Package, f).impacted_nodes.size(),
             std::size_t{2});
}

// Seeds arrive as changed files; the query is asked at package level. The projection
// has to happen inside the engine or every caller reimplements it.
TEST(seeds_are_projected_up_to_the_requested_level) {
    GraphStore store;
    store.reset(chain_snapshot());
    auto r = sim::compute(store, {"file:a/src/x.ts"}, Level::Package, {});
    CHECK_EQ(r.seed_nodes.size(), std::size_t{1});
    CHECK_EQ(r.seed_nodes[0], std::string("pkg:a"));
    CHECK_EQ(r.impacted_nodes.size(), std::size_t{3});
}

// Results for different levels coexist, so switching abstraction is lossless (FR-30).
TEST(impact_results_for_different_levels_coexist) {
    GraphStore store;
    store.reset(chain_snapshot());

    auto push = [&](Level l) {
        ImpactResult r;
        r.level = l;
        r.impacted_nodes.push_back(ImpactedNode{"pkg:a", 0, false, true, Freshness::Current,
                                                ImpactCause::Implementation, {}, false});
        Event e;
        e.type       = EventType::ImpactUpdated;
        e.generation = 101;
        e.payload    = r;
        store.on_event(e);
    };
    push(Level::Package);
    push(Level::File);

    CHECK(store.impact(Level::Package) != nullptr);
    CHECK(store.impact(Level::File) != nullptr);
    CHECK(store.impact(Level::Symbol) == nullptr);
}

// Dirty sets are what keep layout local. A pure change-list update must not claim the
// topology moved, or every file save triggers a full re-layout.
TEST(a_file_changed_event_does_not_dirty_the_topology) {
    GraphStore store;
    store.reset(chain_snapshot());
    store.clear_dirty();

    Event e;
    e.type       = EventType::FileChanged;
    e.generation = 101;
    e.payload    = FileChangedPayload{"a/src/x.ts", "file:a/src/x.ts",
                                      FileChangeKind::Modified, "", Processing::Pending};
    store.on_event(e);

    CHECK(store.dirty().changes);
    CHECK(!store.dirty().topology);
    CHECK(store.dirty().nodes.empty());
}

TEST(adding_an_edge_dirties_topology_but_updating_one_does_not) {
    GraphStore store;
    store.reset(chain_snapshot());
    store.clear_dirty();

    GraphUpdatedPayload upd;
    Edge e   = mk_edge("e:b->a", EdgeKind::DependsOn, "pkg:b", "pkg:a");
    e.freshness   = Freshness::Stale;
    upd.updated_edges = {e};
    store.on_event(graph_event(101, upd));
    CHECK(!store.dirty().topology);
    CHECK(store.dirty().edges.count("e:b->a") == 1);

    store.clear_dirty();
    GraphUpdatedPayload add;
    add.added_edges = {mk_edge("e:c->a", EdgeKind::DependsOn, "pkg:c", "pkg:a")};
    store.on_event(graph_event(102, add));
    CHECK(store.dirty().topology);
}

TEST(changed_file_list_keeps_the_original_change_kind_across_tier_updates) {
    GraphStore store;
    store.reset(chain_snapshot());

    auto fire = [&](FileChangeKind k, Processing p) {
        Event e;
        e.type       = EventType::FileChanged;
        e.generation = 101;
        e.payload    = FileChangedPayload{"a/src/x.ts", "file:a/src/x.ts", k, "", p};
        store.on_event(e);
    };
    fire(FileChangeKind::Created, Processing::Pending);
    fire(FileChangeKind::Modified, Processing::Structural);
    fire(FileChangeKind::Modified, Processing::Settled);

    CHECK_EQ(store.changed_files().size(), std::size_t{1});
    CHECK(store.changed_files()[0].change == FileChangeKind::Created);
    CHECK(store.changed_files()[0].processing == Processing::Settled);
}

// A typo in a fixture must fail at load, not silently render the wrong graph.
TEST(an_unknown_enum_token_is_a_load_error) {
    const std::string text = R"({"schema":"rgv.snapshot/1","generation":1,
        "nodes":[{"id":"n","kind":"packge"}]})";
    bool threw = false;
    try {
        fixture::parse_snapshot(text, "inline");
    } catch (const fixture::ParseError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(a_missing_required_field_is_a_load_error) {
    bool threw = false;
    try {
        fixture::parse_snapshot(R"({"schema":"rgv.snapshot/1"})", "inline");
    } catch (const fixture::ParseError&) {
        threw = true;
    }
    CHECK(threw);
}

// Events authored out of order still replay as a timeline, and same-instant events
// keep the order they were written in so a burst stays readable.
TEST(scenario_events_are_sorted_stably_by_time) {
    const std::string text =
        "{\"t_ms\":300,\"type\":\"file.changed\",\"path\":\"c\"}\n"
        "{\"t_ms\":100,\"type\":\"file.changed\",\"path\":\"a\"}\n"
        "{\"t_ms\":100,\"type\":\"file.changed\",\"path\":\"b\"}\n";
    auto ev = fixture::parse_scenario(text, "inline");
    CHECK_EQ(ev.size(), std::size_t{3});
    CHECK_EQ(ev[0].as<FileChangedPayload>().path, std::string("a"));
    CHECK_EQ(ev[1].as<FileChangedPayload>().path, std::string("b"));
    CHECK_EQ(ev[2].as<FileChangedPayload>().path, std::string("c"));
}

TEST(scenario_comments_and_blank_lines_are_ignored) {
    const std::string text =
        "// a note about this scenario\n"
        "\n"
        "{\"t_ms\":0,\"type\":\"file.changed\",\"path\":\"a\"}\n";
    CHECK_EQ(fixture::parse_scenario(text, "inline").size(), std::size_t{1});
}
