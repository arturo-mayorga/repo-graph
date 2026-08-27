// Architectural specificity (IDF) and the hub-change alert.
#include "TestMain.h"


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
