// Scene rules: what appears on screen, and how state is turned into style.
//
// These are the rules a renderer is most likely to get quietly wrong -- a stale
// conclusion drawn as current looks fine and is a lie -- so they are asserted against
// the components rather than trusted to a visual check.
#include "TestMain.h"

#include "rgv/ecs/LayoutSystem.h"
#include "rgv/ecs/Scene.h"
#include "rgv/model/GraphStore.h"

using namespace rgv;

namespace {

Node mk_node(std::string id, NodeKind k, std::string parent = {}, std::string name = {}) {
    Node n;
    n.id     = std::move(id);
    n.kind   = k;
    n.name   = name.empty() ? n.id : name;
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

// repo -> a, b, c packages; c -> b -> a. Package a owns one file.
Snapshot chain() {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:a", NodeKind::Package, "repo"),
               mk_node("pkg:b", NodeKind::Package, "repo"),
               mk_node("pkg:c", NodeKind::Package, "repo"),
               mk_node("dir:a", NodeKind::Directory, "pkg:a"),
               mk_node("file:a/x.ts", NodeKind::File, "dir:a"),
               mk_node("file:b/y.ts", NodeKind::File, "pkg:b")};
    s.edges = {mk_edge("e:b->a", EdgeKind::DependsOn, "pkg:b", "pkg:a"),
               mk_edge("e:c->b", EdgeKind::DependsOn, "pkg:c", "pkg:b"),
               mk_edge("e:y->x", EdgeKind::Imports, "file:b/y.ts", "file:a/x.ts")};
    return s;
}

void push_impact(GraphStore& store, Level level, std::vector<ImpactedNode> nodes,
                 std::vector<NodeId> seeds = {"pkg:a"}) {
    ImpactResult r;
    r.level          = level;
    r.seed_nodes     = std::move(seeds);
    r.impacted_nodes = std::move(nodes);
    Event e;
    e.type       = EventType::ImpactUpdated;
    e.generation = 101;
    e.payload    = r;
    store.on_event(e);
}

int count_nodes(ecs::Scene& s) {
    int n = 0;
    for ([[maybe_unused]] auto&& row : s.registry.view<const ecs::NodeRef>().each()) ++n;
    return n;
}

} // namespace

// Architecture shows packages, File graph shows files, Filesystem shows the tree.
// Getting this wrong means a view mode silently renders the wrong universe.
TEST(view_mode_selects_which_nodes_exist_on_screen) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene scene;

    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    CHECK_EQ(count_nodes(scene), 3);   // three packages, no repo/dir/file

    scene.view.mode = ecs::ViewMode::FileGraph;
    scene.rebuild(store);
    CHECK_EQ(count_nodes(scene), 2);   // two files

    scene.view.mode = ecs::ViewMode::Filesystem;
    scene.rebuild(store);
    CHECK_EQ(count_nodes(scene), 6);   // 3 packages + 1 directory + 2 files, no repo root
}

// Structural edges are drawn as hierarchy in the filesystem view and must not leak
// dependency arrows into it.
TEST(filesystem_view_renders_containment_not_dependencies) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene scene;
    scene.view.mode = ecs::ViewMode::Filesystem;
    scene.rebuild(store);

    int contains = 0, other = 0;
    for (auto [e, ref] : scene.registry.view<const ecs::EdgeRef>().each()) {
        (ref.kind == EdgeKind::Contains) ? ++contains : ++other;
    }
    CHECK(contains > 0);
    CHECK_EQ(other, 0);
}

// THE rule. The impact result reports how trustworthy the PATH is, which can be worse
// than anything the node says about itself. Styling from node freshness alone renders
// a stale conclusion as current, which NFR-04 forbids.
TEST(a_stale_impact_conclusion_is_styled_as_stale_even_when_the_node_is_current) {
    GraphStore store;
    store.reset(chain());
    CHECK(store.node("pkg:b")->freshness == Freshness::Current);

    push_impact(store, Level::Package,
                {ImpactedNode{"pkg:b", 1, true, false, Freshness::Stale,
                              ImpactCause::Implementation, {}, false}});

    ecs::Scene scene;
    scene.view.level = Level::Package;
    scene.rebuild(store);

    const entt::entity e = scene.find_node("pkg:b");
    CHECK(e != entt::null);
    const auto& style = scene.registry.get<ecs::Style>(e);
    // A dashed outline in the stale colour: two channels, because colour alone is not
    // enough to stop someone trusting the result.
    CHECK(style.dash > 0.0f);
    CHECK_EQ(scene.stats.stale, 1);
}

TEST(a_current_impact_conclusion_is_not_marked_uncertain) {
    GraphStore store;
    store.reset(chain());
    push_impact(store, Level::Package,
                {ImpactedNode{"pkg:b", 1, true, false, Freshness::Current,
                              ImpactCause::Implementation, {}, false}});

    ecs::Scene scene;
    scene.view.level = Level::Package;
    scene.rebuild(store);

    const auto& style = scene.registry.get<ecs::Style>(scene.find_node("pkg:b"));
    CHECK_EQ(style.dash, 0.0f);
    CHECK_EQ(scene.stats.stale, 0);
}

// A changed file must make its owning package read as changed, or the architecture
// view shows nothing at all while the agent is working.
TEST(a_changed_file_marks_its_owning_package_as_changed) {
    GraphStore store;
    store.reset(chain());

    Event e;
    e.type       = EventType::FileChanged;
    e.generation = 101;
    e.payload    = FileChangedPayload{"a/x.ts", "file:a/x.ts", FileChangeKind::Modified, "",
                                      Processing::Pending};
    store.on_event(e);

    ecs::Scene scene;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);

    CHECK(scene.registry.all_of<ecs::Changed>(scene.find_node("pkg:a")));
    CHECK(!scene.registry.all_of<ecs::Changed>(scene.find_node("pkg:b")));
    CHECK_EQ(scene.stats.changed, 1);
}

// "Show me only what the agent touched plus affected context" (FR-35).
TEST(hiding_unaffected_nodes_keeps_changed_and_impacted_only) {
    GraphStore store;
    store.reset(chain());

    Event e;
    e.type       = EventType::FileChanged;
    e.generation = 101;
    e.payload    = FileChangedPayload{"a/x.ts", "file:a/x.ts", FileChangeKind::Modified, "",
                                      Processing::Pending};
    store.on_event(e);
    push_impact(store, Level::Package,
                {ImpactedNode{"pkg:a", 0, false, true, Freshness::Current,
                              ImpactCause::Implementation, {}, false},
                 ImpactedNode{"pkg:b", 1, true, false, Freshness::Current,
                              ImpactCause::Implementation, {}, false}});

    ecs::Scene scene;
    scene.view.mode                    = ecs::ViewMode::Architecture;
    scene.view.level                   = Level::Package;
    scene.view.filters.show_unaffected = false;
    scene.rebuild(store);

    CHECK(scene.find_node("pkg:a") != entt::null);
    CHECK(scene.find_node("pkg:b") != entt::null);
    CHECK(scene.find_node("pkg:c") == entt::null);   // untouched, unimpacted
}

TEST(the_text_filter_matches_name_or_path) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene scene;
    scene.view.mode         = ecs::ViewMode::Architecture;
    scene.view.filters.text = "pkg:b";
    scene.rebuild(store);
    CHECK_EQ(count_nodes(scene), 1);
}

// Selecting an impacted node lights the chain that explains it. If the hop marks are
// wrong the canvas highlights a path the inspector is not describing.
TEST(the_explained_path_marks_every_node_and_edge_on_the_chain) {
    GraphStore store;
    store.reset(chain());
    push_impact(store, Level::Package,
                {ImpactedNode{"pkg:c", 2, false, false, Freshness::Current,
                              ImpactCause::Implementation,
                              {ImpactPath{{"e:c->b", "e:b->a"}}}, false}});

    ecs::Scene scene;
    scene.view.level         = Level::Package;
    scene.view.mode          = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    scene.view.selected_node = "pkg:c";
    scene.update_explained_path(store);

    auto on_path = [&](const std::string& id, bool edge) {
        const entt::entity e = edge ? scene.find_edge(id) : scene.find_node(id);
        return e != entt::null && scene.registry.all_of<ecs::OnExplainedPath>(e);
    };
    CHECK(on_path("pkg:c", false));
    CHECK(on_path("e:c->b", true));
    CHECK(on_path("pkg:b", false));
    CHECK(on_path("e:b->a", true));
    CHECK(on_path("pkg:a", false));
}

TEST(clearing_the_selection_clears_the_explained_path) {
    GraphStore store;
    store.reset(chain());
    push_impact(store, Level::Package,
                {ImpactedNode{"pkg:c", 2, false, false, Freshness::Current,
                              ImpactCause::Implementation,
                              {ImpactPath{{"e:c->b", "e:b->a"}}}, false}});
    ecs::Scene scene;
    scene.view.level = Level::Package;
    scene.rebuild(store);
    scene.view.selected_node = "pkg:c";
    scene.update_explained_path(store);
    scene.view.selected_node.clear();
    scene.update_explained_path(store);

    int marked = 0;
    for ([[maybe_unused]] auto&& row : scene.registry.view<const ecs::OnExplainedPath>().each()) ++marked;
    CHECK_EQ(marked, 0);
}

// Depth 0 = depends on nothing in view, and rows go upward from there, so a blast
// radius reads bottom-to-top the way the spec draws it.
TEST(layout_puts_dependencies_below_their_dependents) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);

    auto depth_of = [&](const char* id) {
        return scene.registry.get<ecs::Depth>(scene.find_node(id)).value;
    };
    CHECK_EQ(depth_of("pkg:a"), 0);
    CHECK_EQ(depth_of("pkg:b"), 1);
    CHECK_EQ(depth_of("pkg:c"), 2);

    auto y_of = [&](const char* id) {
        return scene.registry.get<ecs::LayoutTarget>(scene.find_node(id)).p.y;
    };
    CHECK(y_of("pkg:c") < y_of("pkg:b"));   // screen y grows downward
    CHECK(y_of("pkg:b") < y_of("pkg:a"));
}

// Import graphs really do cycle. Layout must terminate and stay finite.
TEST(layout_survives_a_dependency_cycle) {
    Snapshot s = chain();
    s.edges.push_back(mk_edge("e:a->c", EdgeKind::DependsOn, "pkg:a", "pkg:c"));
    GraphStore store;
    store.reset(s);

    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);

    for (auto [e, ref, d] : scene.registry.view<const ecs::NodeRef, const ecs::Depth>().each()) {
        CHECK(d.value >= 0);
        CHECK(d.value < 64);
    }
}

// Layout must not throw away positions when the graph changes, or every file save
// reshuffles the screen (spec 11.2).
TEST(adding_a_node_does_not_move_the_existing_ones) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);
    for (int i = 0; i < 200; ++i) layout.step(scene, 1.0f / 60.0f);
    CHECK(layout.settled());

    const Vec2 before = scene.registry.get<ecs::Position>(scene.find_node("pkg:c")).p;

    GraphUpdatedPayload p;
    p.added_nodes = {mk_node("pkg:d", NodeKind::Package, "repo")};
    p.added_edges = {mk_edge("e:d->a", EdgeKind::DependsOn, "pkg:d", "pkg:a")};
    Event ev;
    ev.type       = EventType::GraphUpdated;
    ev.generation = 102;
    ev.payload    = p;
    store.on_event(ev);

    scene.sync(store);
    layout.reset(scene, store);
    for (int i = 0; i < 200; ++i) layout.step(scene, 1.0f / 60.0f);

    const Vec2 after = scene.registry.get<ecs::Position>(scene.find_node("pkg:c")).p;
    CHECK(std::abs(after.y - before.y) < 1.0f);   // same row
    CHECK(scene.find_node("pkg:d") != entt::null);
}

TEST(a_pinned_node_is_left_alone_by_layout) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);

    const entt::entity e = scene.find_node("pkg:b");
    scene.registry.emplace<ecs::Pinned>(e);
    scene.registry.get<ecs::Position>(e).p = Vec2{999.0f, -999.0f};

    layout.reset(scene, store);
    for (int i = 0; i < 100; ++i) layout.step(scene, 1.0f / 60.0f);

    const Vec2 p = scene.registry.get<ecs::Position>(e).p;
    CHECK_EQ(p.x, 999.0f);
    CHECK_EQ(p.y, -999.0f);
}

// Picking has to agree with the camera transform, or clicks land on the wrong node.
TEST(picking_agrees_with_the_camera_transform) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);
    for (int i = 0; i < 200; ++i) layout.step(scene, 1.0f / 60.0f);

    scene.view.camera.vw = 1200.0f;
    scene.view.camera.vh = 800.0f;
    scene.view.free_size = Vec2{1200.0f, 800.0f};
    scene.focus_on({});

    const entt::entity target = scene.find_node("pkg:b");
    const Vec2 centre = scene.registry.get<ecs::Position>(target).p;
    CHECK(scene.pick(scene.view.camera.world_to_screen(centre)) == target);

    // Far outside every node.
    CHECK(scene.pick(scene.view.camera.world_to_screen(centre + Vec2{9000.0f, 9000.0f})) ==
          entt::null);
}

// -- semantic zoom -----------------------------------------------------------

// Below the point where a label is readable, a labelled box is a smear. The node
// becomes a dot and the hover card takes over naming it.
TEST(labels_switch_off_once_they_would_be_illegible) {
    ecs::ViewState view;
    view.graph_text_scale = 1.0f;

    view.camera.zoom = 1.0f;
    CHECK(ecs::node_detail(view).labels);
    CHECK_EQ(ecs::node_detail(view).t, 1.0f);

    view.camera.zoom = 0.15f;   // 13px * 0.15 = ~2px of text
    CHECK(!ecs::node_detail(view).labels);
    CHECK_EQ(ecs::node_detail(view).t, 0.0f);
}

// A bigger text preference should keep labels alive further out, because the text is
// genuinely still readable there.
TEST(a_larger_text_scale_keeps_labels_readable_at_lower_zoom) {
    ecs::ViewState small, large;
    small.graph_text_scale = 1.0f;
    large.graph_text_scale = 2.0f;
    small.camera.zoom = large.camera.zoom = 0.45f;

    CHECK(ecs::node_detail(large).t > ecs::node_detail(small).t);
}

// The collapsed dot holds a constant SCREEN size. If it scaled with the world it
// would vanish at overview zoom, which is exactly where it is needed.
TEST(a_collapsed_node_holds_a_constant_screen_size) {
    ecs::ViewState view;
    view.graph_text_scale = 1.0f;
    const Vec2 layout_half{120.0f, 21.0f};

    auto screen_h = [&](float zoom) {
        view.camera.zoom = zoom;
        return ecs::render_half(view, ecs::node_detail(view), layout_half, 6.0f).y * zoom;
    };
    // Two very different overview zooms, same size on screen.
    CHECK(std::abs(screen_h(0.10f) - screen_h(0.05f)) < 0.01f);
    CHECK(std::abs(screen_h(0.10f) - 6.0f) < 0.01f);
}

TEST(a_fully_zoomed_in_node_is_drawn_at_its_layout_footprint) {
    ecs::ViewState view;
    view.camera.zoom = 2.0f;
    const Vec2 layout_half{120.0f, 21.0f};
    const Vec2 drawn = ecs::render_half(view, ecs::node_detail(view), layout_half, 6.0f);
    CHECK(std::abs(drawn.x - layout_half.x) < 0.01f);
    CHECK(std::abs(drawn.y - layout_half.y) < 0.01f);
}

// What the eye should be drawn to is bigger at overview scale, where colour and size
// are the only channels left.
TEST(changed_and_impacted_nodes_get_larger_dots_than_context) {
    const float context  = ecs::dot_px_for(false, false, false);
    const float impacted = ecs::dot_px_for(false, true, false);
    const float changed  = ecs::dot_px_for(true, true, false);
    CHECK(impacted > context);
    CHECK(changed > impacted);
    CHECK(ecs::dot_px_for(false, false, true) > context);   // selected or hovered
}

// Layout must not reflow when the user zooms. The drawn size collapses; the footprint
// layout reserved does not.
TEST(zooming_does_not_change_the_layout_footprint) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);

    const entt::entity e      = scene.find_node("pkg:b");
    const Vec2         before = scene.registry.get<ecs::Extent>(e).half;

    scene.view.camera.zoom = 0.05f;
    layout.reset(scene, store);
    const Vec2 after = scene.registry.get<ecs::Extent>(e).half;

    CHECK_EQ(before.x, after.x);
    CHECK_EQ(before.y, after.y);
}

// Clicks have to land on what is actually drawn. A dot only a few pixels across still
// needs to be hittable, or overview zoom becomes a test of mouse precision.
TEST(a_collapsed_dot_is_still_clickable) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);
    for (int i = 0; i < 200; ++i) layout.step(scene, 1.0f / 60.0f);

    scene.view.camera.vw = 1200.0f;
    scene.view.camera.vh = 800.0f;
    scene.view.free_size = Vec2{1200.0f, 800.0f};
    scene.focus_on({});
    scene.view.camera.zoom = 0.06f;   // well past the point where labels are gone
    CHECK(!ecs::node_detail(scene.view).labels);

    const entt::entity target = scene.find_node("pkg:b");
    const Vec2         centre = scene.registry.get<ecs::Position>(target).p;
    CHECK(scene.pick(scene.view.camera.world_to_screen(centre)) == target);

    // A few pixels off-centre still hits, because the hit area has a screen floor.
    const Vec2 near_miss = scene.view.camera.world_to_screen(centre) + Vec2{4.0f, 4.0f};
    CHECK(scene.pick(near_miss) == target);
}

// Node boxes are sized to hold their label, so the text preference changes geometry.
TEST(graph_text_scale_resizes_node_boxes_without_losing_positions) {
    GraphStore store;
    store.reset(chain());
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    scene.view.mode = ecs::ViewMode::Architecture;
    scene.rebuild(store);
    layout.reset(scene, store);
    for (int i = 0; i < 200; ++i) layout.step(scene, 1.0f / 60.0f);

    const entt::entity e       = scene.find_node("pkg:b");
    const Vec2         before  = scene.registry.get<ecs::Extent>(e).half;
    const float        row_y   = scene.registry.get<ecs::Position>(e).p.y;

    scene.view.graph_text_scale = 2.0f;
    scene.refresh_extents();

    const Vec2 after = scene.registry.get<ecs::Extent>(e).half;
    CHECK(after.x > before.x);
    CHECK(after.y > before.y);
    CHECK(scene.needs_layout_reset);

    // Re-laying out must keep the node in its row rather than reshuffling the graph
    // under the user's slider.
    layout.reset(scene, store);
    for (int i = 0; i < 200; ++i) layout.step(scene, 1.0f / 60.0f);
    CHECK(std::abs(scene.registry.get<ecs::Position>(e).p.y - row_y) < 1.0f);
}
