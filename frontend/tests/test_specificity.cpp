// Architectural specificity (IDF) and the hub-change alert.
#include "TestMain.h"


#include "rgv/analysis/Reach.h"
#include "rgv/analysis/Specificity.h"
#include "Harness.h"

#include "rgv/model/GraphStore.h"

#include <cmath>

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

// Four packages. Everything depends on `log`; only `api` depends on `auth`.
// log is the stop word, auth is the content word.
Snapshot hub_graph() {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:log", NodeKind::Package, "repo"),
               mk_node("pkg:auth", NodeKind::Package, "repo"),
               mk_node("pkg:api", NodeKind::Package, "repo"),
               mk_node("pkg:web", NodeKind::Package, "repo")};
    s.edges = {mk_edge("e:auth->log", EdgeKind::DependsOn, "pkg:auth", "pkg:log"),
               mk_edge("e:api->log", EdgeKind::DependsOn, "pkg:api", "pkg:log"),
               mk_edge("e:web->log", EdgeKind::DependsOn, "pkg:web", "pkg:log"),
               mk_edge("e:api->auth", EdgeKind::DependsOn, "pkg:api", "pkg:auth"),
               mk_edge("e:web->api", EdgeKind::DependsOn, "pkg:web", "pkg:api")};
    return s;
}

analysis::SpecificityIndex index_of(const GraphStore& store) {
    return analysis::build(store, Level::Package, ImpactFilters{});
}

ImpactedNode impacted(std::string id, int distance, std::vector<std::string> path) {
    ImpactedNode n;
    n.node_id      = std::move(id);
    n.min_distance = distance;
    n.direct       = distance == 1;
    n.changed      = distance == 0;
    if (!path.empty()) n.paths.push_back(ImpactPath{std::move(path)});
    return n;
}

} // namespace

// The whole point: a package everything depends on scores near zero, one with a single
// dependent scores high.
TEST(a_package_everything_depends_on_scores_near_zero) {
    GraphStore store;
    store.reset(hub_graph());
    const auto idx = index_of(store);

    CHECK_EQ(idx.population(), 4);
    CHECK_EQ(idx.dependents("pkg:log"), 3);
    CHECK_EQ(idx.dependents("pkg:auth"), 1);

    // log(4/3)/log(4) = 0.2075 ; log(4/1)/log(4) = 1.0
    CHECK(idx.specificity("pkg:log") < 0.25f);
    CHECK(std::abs(idx.specificity("pkg:auth") - 1.0f) < 1e-4f);
    CHECK(idx.specificity("pkg:log") < idx.specificity("pkg:api"));
}

TEST(specificity_stays_within_zero_and_one) {
    GraphStore store;
    store.reset(hub_graph());
    const auto idx = index_of(store);
    for (const char* id : {"pkg:log", "pkg:auth", "pkg:api", "pkg:web"}) {
        const float s = idx.specificity(id);
        CHECK(s >= 0.0f);
        CHECK(s <= 1.0f);
    }
}

// A node nothing depends on cannot be a hub, and must not be discounted.
TEST(a_node_with_no_dependents_is_maximally_specific) {
    GraphStore store;
    store.reset(hub_graph());
    const auto idx = index_of(store);
    CHECK_EQ(idx.dependents("pkg:web"), 0);
    CHECK(std::abs(idx.specificity("pkg:web") - 1.0f) < 1e-4f);
}

// Containment is not dependency. Counting it would make every directory a hub.
TEST(structural_edges_do_not_count_toward_document_frequency) {
    Snapshot s = hub_graph();
    s.edges.push_back(mk_edge("e:contains", EdgeKind::Contains, "pkg:web", "pkg:auth"));
    GraphStore store;
    store.reset(s);
    CHECK_EQ(index_of(store).dependents("pkg:auth"), 1);   // not 2
}

// Two files in one package importing the same thing is one architectural dependency.
TEST(document_frequency_counts_distinct_dependents_not_edges) {
    Snapshot s = hub_graph();
    s.edges.push_back(mk_edge("e:api->auth-2", EdgeKind::DependsOn, "pkg:api", "pkg:auth"));
    GraphStore store;
    store.reset(s);
    CHECK_EQ(index_of(store).dependents("pkg:auth"), 1);
}

TEST(a_single_node_population_carries_no_comparative_information) {
    Snapshot s;
    s.nodes = {mk_node("pkg:only", NodeKind::Package)};
    GraphStore store;
    store.reset(s);
    const auto idx = index_of(store);
    CHECK_EQ(idx.population(), 1);
    CHECK(std::abs(idx.specificity("pkg:only") - 1.0f) < 1e-4f);
}

// The weakest link rule: one hop through a hub makes the whole explanation weak.
TEST(relevance_is_the_weakest_specificity_along_the_path) {
    GraphStore store;
    store.reset(hub_graph());
    const auto idx = index_of(store);

    // web -> api -> auth : both hops land on specific packages
    const float via_auth =
        analysis::relevance(store, idx, impacted("pkg:web", 2, {"e:web->api", "e:api->auth"}));
    // web -> log : lands on the hub
    const float via_log = analysis::relevance(store, idx, impacted("pkg:web", 1, {"e:web->log"}));

    CHECK(via_log < via_auth);
    CHECK(std::abs(via_log - idx.specificity("pkg:log")) < 1e-4f);
}

// Several independent reasons: the strongest one stands, not the weakest.
TEST(the_strongest_of_several_paths_decides_relevance) {
    GraphStore store;
    store.reset(hub_graph());
    const auto idx = index_of(store);

    ImpactedNode n = impacted("pkg:web", 1, {"e:web->log"});           // weak
    n.paths.push_back(ImpactPath{{"e:web->api", "e:api->auth"}});      // strong
    CHECK(analysis::relevance(store, idx, n) > idx.specificity("pkg:log"));
}

// A seed is the change, not an inference from it. It can never be discounted -- this
// is what stops a hub edit being filtered away by its own low score.
TEST(a_seed_is_never_discounted_by_its_own_specificity) {
    GraphStore store;
    store.reset(hub_graph());
    const auto idx = index_of(store);

    ImpactedNode seed = impacted("pkg:log", 0, {});
    CHECK(idx.specificity("pkg:log") < 0.25f);
    CHECK_EQ(analysis::relevance(store, idx, seed), 1.0f);
}

// Never hide what you could not evaluate.
TEST(an_unexplained_impact_is_not_discounted) {
    GraphStore store;
    store.reset(hub_graph());
    CHECK_EQ(analysis::relevance(store, index_of(store), impacted("pkg:web", 2, {})), 1.0f);
}

// -- the alert ---------------------------------------------------------------

TEST(changing_a_hub_raises_an_alert_with_its_reach) {
    GraphStore store;
    store.reset(hub_graph());
    const auto idx = index_of(store);

    ImpactResult r;
    r.level      = Level::Package;
    r.seed_nodes = {"pkg:log"};
    r.impacted_nodes = {impacted("pkg:log", 0, {}),
                        impacted("pkg:auth", 1, {"e:auth->log"}),
                        impacted("pkg:api", 1, {"e:api->log"}),
                        impacted("pkg:web", 1, {"e:web->log"})};

    const auto alerts = analysis::hub_seeds(store, idx, r, analysis::kHubThreshold);
    CHECK_EQ(alerts.size(), std::size_t{1});
    CHECK_EQ(alerts[0].node, std::string("pkg:log"));
    CHECK_EQ(alerts[0].dependents, 3);
    CHECK_EQ(alerts[0].population, 4);
    CHECK_EQ(alerts[0].reach, 3);
    CHECK(alerts[0].reach_fraction > 0.7f);
}

TEST(changing_an_ordinary_package_raises_no_alert) {
    GraphStore store;
    store.reset(hub_graph());

    ImpactResult r;
    r.level          = Level::Package;
    r.seed_nodes     = {"pkg:auth"};
    r.impacted_nodes = {impacted("pkg:auth", 0, {}), impacted("pkg:api", 1, {"e:api->auth"})};

    CHECK(analysis::hub_seeds(store, index_of(store), r, analysis::kHubThreshold).empty());
}

// -- the filter, through the scene -------------------------------------------

namespace {
void push_impact(GraphStore& store, ImpactResult r) {
    Event e;
    e.type       = EventType::ImpactUpdated;
    e.generation = 101;
    e.payload    = std::move(r);
    store.on_event(e);
}

void push_change(GraphStore& store, const std::string& node_id) {
    Event e;
    e.type       = EventType::FileChanged;
    e.generation = 101;
    e.payload    = FileChangedPayload{node_id, node_id, FileChangeKind::Modified, "",
                                      Processing::Settled};
    store.on_event(e);
}
} // namespace

TEST(raising_the_relevance_threshold_mutes_impact_that_runs_through_a_hub) {
    GraphStore store;
    store.reset(hub_graph());

    ImpactResult r;
    r.level          = Level::Package;
    r.seed_nodes     = {"pkg:log"};
    r.impacted_nodes = {impacted("pkg:log", 0, {}),
                        impacted("pkg:auth", 1, {"e:auth->log"}),
                        impacted("pkg:api", 1, {"e:api->log"}),
                        impacted("pkg:web", 1, {"e:web->log"})};
    push_impact(store, r);

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);
    h.view().mode  = ecs::ViewMode::Architecture;
    h.view().level = Level::Package;

    h.filters().min_relevance = 0.0f;
    h.request_rebuild();
    h.tick();
    CHECK_EQ(h.stats().impacted, 3);
    CHECK_EQ(h.stats().muted, 0);

    h.filters().min_relevance = 0.5f;
    h.request_rebuild();
    h.tick();
    CHECK_EQ(h.stats().impacted, 0);   // every path runs through the hub
    CHECK_EQ(h.stats().muted, 3);
}

// The rule that makes the whole thing safe: a hub edit is the loudest event there is,
// so the filter must never be able to suppress it.
TEST(the_relevance_filter_never_mutes_the_changed_node_itself) {
    GraphStore store;
    store.reset(hub_graph());
    push_change(store, "pkg:log");

    ImpactResult r;
    r.level          = Level::Package;
    r.seed_nodes     = {"pkg:log"};
    r.impacted_nodes = {impacted("pkg:log", 0, {}), impacted("pkg:web", 1, {"e:web->log"})};
    push_impact(store, r);

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);
    h.view().mode             = ecs::ViewMode::Architecture;
    h.view().level            = Level::Package;
    h.filters().min_relevance = 1.0f;   // maximum filtering
    h.request_rebuild();
    h.tick();

    const entt::entity e = h.node("pkg:log");
    CHECK(e != entt::null);
    CHECK(h.registry().all_of<ecs::Changed>(e));
    const auto* imp = h.registry().try_get<ecs::Impacted>(e);
    CHECK(imp != nullptr);
    CHECK(!imp->muted);
    CHECK_EQ(h.stats().changed, 1);
}

TEST(a_changed_hub_is_flagged_on_the_node_itself) {
    GraphStore store;
    store.reset(hub_graph());
    push_change(store, "pkg:log");

    ImpactResult r;
    r.level          = Level::Package;
    r.seed_nodes     = {"pkg:log"};
    r.impacted_nodes = {impacted("pkg:log", 0, {}),
                        impacted("pkg:auth", 1, {"e:auth->log"}),
                        impacted("pkg:api", 1, {"e:api->log"}),
                        impacted("pkg:web", 1, {"e:web->log"})};
    push_impact(store, r);

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);
    h.view().mode  = ecs::ViewMode::Architecture;
    h.view().level = Level::Package;
    h.request_rebuild();
    h.tick();

    const auto& derived = h.world.resource<ecs::DerivedState>();
    CHECK_EQ(derived.hub_alerts.size(), std::size_t{1});
    const auto* hub = h.registry().try_get<ecs::HubSeed>(h.node("pkg:log"));
    CHECK(hub != nullptr);
    CHECK_EQ(hub->dependents, 3);
    CHECK(hub->reach_fraction > 0.7f);

    // An ordinary changed package carries no such flag.
    CHECK(h.registry().try_get<ecs::HubSeed>(h.node("pkg:auth")) == nullptr);
}

// -- stop-word removal: hiding the hubs themselves ---------------------------

// The point of the whole IDF exercise. A package most of the repository depends on is
// the architectural equivalent of the word "the": it clutters every view and explains
// nothing, so above a threshold it should leave the picture entirely -- not merely be
// drawn dimmer.
TEST(raising_the_relevance_threshold_hides_hub_packages_entirely) {
    GraphStore store;
    store.reset(hub_graph());

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);
    h.view().mode  = ecs::ViewMode::Architecture;
    h.view().level = Level::Package;

    // log: 3 of 4 packages depend on it -> specificity ~0.21
    // auth: 1 dependent -> specificity 1.0
    h.filters().min_relevance = 0.0f;
    h.request_rebuild();
    h.tick();
    CHECK(h.node("pkg:log") != entt::null);

    h.filters().min_relevance = 0.5f;
    h.request_rebuild();
    h.tick();
    CHECK(h.node("pkg:log") == entt::null);    // the stop word is gone
    CHECK(h.node("pkg:auth") != entt::null);   // the content word stays
}

// Edges into a hidden node must go with it, or the renderer walks into an endpoint
// that no longer exists.
TEST(hiding_a_hub_removes_the_edges_that_pointed_at_it) {
    // As files and imports, which is what the dependency views draw now. The rule
    // under test is SceneSyncSystem's, and it does not care which kind vanished.
    Snapshot s = hub_graph();
    for (auto& n : s.nodes) {
        if (n.kind == NodeKind::Package) n.kind = NodeKind::File;
    }
    for (auto& e : s.edges) {
        if (e.kind == EdgeKind::DependsOn) e.kind = EdgeKind::Imports;
    }
    GraphStore store;
    store.reset(s);

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);
    h.view().mode             = ecs::ViewMode::FileGraph;
    h.view().level            = Level::File;
    h.filters().min_relevance = 0.5f;
    h.request_rebuild();
    h.tick();

    CHECK(h.edge("e:auth->log") == entt::null);
    CHECK(h.edge("e:api->auth") != entt::null);
}

// The rule that keeps this safe. A hub edit is the loudest event the product can
// report, so the filter that hides hubs must never hide one that just changed.
TEST(a_changed_hub_is_never_hidden_however_high_the_filter) {
    GraphStore store;
    store.reset(hub_graph());

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);
    push_change(h.store(), "pkg:log");

    h.view().mode             = ecs::ViewMode::Architecture;
    h.view().level            = Level::Package;
    h.filters().min_relevance = 1.0f;   // maximum filtering
    h.request_rebuild();
    h.tick();

    CHECK(h.node("pkg:log") != entt::null);
    CHECK(h.registry().all_of<ecs::Changed>(h.node("pkg:log")));
}

// A seed of the current impact result is what everything else is explained against;
// hiding it would leave paths pointing at nothing.
TEST(an_impact_seed_is_never_hidden_by_the_filter) {
    GraphStore store;
    store.reset(hub_graph());

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);

    ImpactResult r;
    r.level          = Level::Package;
    r.seed_nodes     = {"pkg:log"};
    r.impacted_nodes = {impacted("pkg:log", 0, {}), impacted("pkg:web", 1, {"e:web->log"})};
    push_impact(h.store(), std::move(r));

    h.view().mode             = ecs::ViewMode::Architecture;
    h.view().level            = Level::Package;
    h.filters().min_relevance = 1.0f;
    h.request_rebuild();
    h.tick();

    CHECK(h.node("pkg:log") != entt::null);
}

// The count must report stop words dropped, not everything the view mode already
// excludes -- it read "26 hidden" on an 8-package repo before this.
TEST(the_hidden_count_reports_stop_words_not_mode_filtered_nodes) {
    GraphStore store;
    store.reset(hub_graph());

    rgvtest::Harness h;
    h.world.resource<GraphStore>() = std::move(store);
    h.view().mode  = ecs::ViewMode::Architecture;
    h.view().level = Level::Package;

    h.filters().min_relevance = 0.0f;
    h.request_rebuild();
    h.tick();
    CHECK_EQ(h.stats().hidden, 0);

    // Only pkg:log scores below 0.5; auth, api and web are all fully specific.
    h.filters().min_relevance = 0.5f;
    h.request_rebuild();
    h.tick();
    CHECK_EQ(h.stats().hidden, 1);
}

// -- reach --------------------------------------------------------------------
//
// How much of the repository ultimately depends on a node. Where specificity asks how
// informative a dependency is, reach asks how far the consequences of touching
// something travel -- and it is what decides how close to the core the concentric
// layout puts a node.

// The whole point: reach counts the chain, not the neighbours. A node imported by one
// adapter that everything else sits behind is core, and a direct count calls it a leaf.
TEST(reach_counts_transitive_dependents_not_direct_ones) {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:core", NodeKind::Package, "repo"),
               mk_node("pkg:adapter", NodeKind::Package, "repo")};
    // One adapter on core; five packages behind the adapter.
    s.edges = {mk_edge("e:ad", EdgeKind::DependsOn, "pkg:adapter", "pkg:core")};
    for (int i = 0; i < 5; ++i) {
        const std::string p = "pkg:app" + std::to_string(i);
        s.nodes.push_back(mk_node(p, NodeKind::Package, "repo"));
        s.edges.push_back(mk_edge("e:" + p, EdgeKind::DependsOn, p, "pkg:adapter"));
    }
    GraphStore store;
    store.reset(s);
    const auto r = analysis::build_reach(store, Level::Package, ImpactFilters{});

    // Direct: core has exactly one dependent, which is the misleading number.
    const auto spec = analysis::build(store, Level::Package, ImpactFilters{});
    CHECK_EQ(spec.dependents("pkg:core"), 1);

    CHECK_EQ(r.dependents("pkg:core"), 6);      // adapter + the five behind it
    CHECK_EQ(r.dependents("pkg:adapter"), 5);
    CHECK_EQ(r.dependents("pkg:app0"), 0);
    CHECK_EQ(r.widest(), 6);
}

// Import graphs cycle. The fixed point is monotone, so it converges rather than
// diverging, and a node is never counted as depending on itself.
TEST(reach_survives_a_cycle) {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:a", NodeKind::Package, "repo"),
               mk_node("pkg:b", NodeKind::Package, "repo"),
               mk_node("pkg:c", NodeKind::Package, "repo")};
    s.edges = {mk_edge("e:ab", EdgeKind::DependsOn, "pkg:a", "pkg:b"),
               mk_edge("e:bc", EdgeKind::DependsOn, "pkg:b", "pkg:c"),
               mk_edge("e:ca", EdgeKind::DependsOn, "pkg:c", "pkg:a")};
    GraphStore store;
    store.reset(s);
    const auto r = analysis::build_reach(store, Level::Package, ImpactFilters{});

    // Each reaches the other two, and never itself.
    CHECK_EQ(r.dependents("pkg:a"), 2);
    CHECK_EQ(r.dependents("pkg:b"), 2);
    CHECK_EQ(r.dependents("pkg:c"), 2);
}

// `ImpactFilters` defaults to DependsOn, which is a package-level edge. At file level
// that matched nothing, so every file scored as having no dependents at all -- which
// silently disabled the relevance filter there and flattened the concentric layout into
// one ring. The level has to pick the edges.
TEST(file_level_analysis_counts_import_edges) {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("file:base.ts", NodeKind::File, "repo"),
               mk_node("file:one.ts", NodeKind::File, "repo"),
               mk_node("file:two.ts", NodeKind::File, "repo")};
    s.edges = {mk_edge("e:1", EdgeKind::Imports, "file:one.ts", "file:base.ts"),
               mk_edge("e:2", EdgeKind::Imports, "file:two.ts", "file:one.ts")};
    GraphStore store;
    store.reset(s);

    ImpactFilters files;
    files.edge_kinds = {EdgeKind::Imports};
    const auto r = analysis::build_reach(store, Level::File, files);
    CHECK_EQ(r.dependents("file:base.ts"), 2);

    // The default policy is package-level and finds nothing here, which is exactly the
    // trap: it does not fail, it quietly reports a flat graph.
    const auto blind = analysis::build_reach(store, Level::File, ImpactFilters{});
    CHECK_EQ(blind.dependents("file:base.ts"), 0);
}
