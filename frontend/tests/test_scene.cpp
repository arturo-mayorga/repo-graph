// Scene rules: what appears on screen, and how state is turned into style.
//
// Driven through the real schedule -- the same systems the application runs, minus the
// platform, the GPU, and the data source. A test that bypassed the systems would not
// be testing the thing that ships.
#include "Harness.h"
#include "TestMain.h"

#include "rgv/model/GraphStore.h"
#include "rgv/view/CameraFit.h"
#include "rgv/view/SemanticZoom.h"

#include <algorithm>

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
    e.payload    = std::move(r);
    store.on_event(e);
}

void push_change(GraphStore& store, const std::string& path, const std::string& node_id) {
    Event e;
    e.type       = EventType::FileChanged;
    e.generation = 101;
    e.payload    = FileChangedPayload{path, node_id, FileChangeKind::Modified, "",
                                      Processing::Pending};
    store.on_event(e);
}

int count_nodes(rgvtest::Harness& h) {
    int n = 0;
    for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::NodeRef>().each()) ++n;
    return n;
}

// Seeds a harness with the chain graph and runs a frame.
rgvtest::Harness make(Snapshot s = chain()) {
    rgvtest::Harness h;
    h.store().reset(s);
    h.view().mode  = ecs::ViewMode::Architecture;
    h.view().level = Level::Package;
    h.request_rebuild();
    h.tick();
    return h;
}

} // namespace

// -- visibility ---------------------------------------------------------------

// Architecture shows packages, File graph shows files, Filesystem shows the tree.
// Getting this wrong means a view mode silently renders the wrong universe.
TEST(view_mode_selects_which_nodes_exist_on_screen) {
    auto h = make();
    CHECK_EQ(count_nodes(h), 3);   // three packages, no repo/dir/file

    h.view().mode = ecs::ViewMode::FileGraph;
    h.tick();
    CHECK_EQ(count_nodes(h), 2);   // two files

    h.view().mode = ecs::ViewMode::Filesystem;
    h.tick();
    // repo + 3 packages + 1 directory + 2 files. The repository is included here and
    // nowhere else: it is the centre the radial layout grows from.
    CHECK_EQ(count_nodes(h), 7);
}

// Structural edges are drawn as hierarchy there, and dependency arrows must not leak in.
TEST(filesystem_view_renders_containment_not_dependencies) {
    auto h        = make();
    h.view().mode = ecs::ViewMode::Filesystem;
    h.tick();

    int contains = 0, other = 0;
    for (auto [e, ref] : h.registry().view<const ecs::EdgeRef>().each()) {
        (ref.kind == EdgeKind::Contains) ? ++contains : ++other;
    }
    CHECK(contains > 0);
    CHECK_EQ(other, 0);
}

TEST(the_text_filter_matches_name_or_path) {
    auto h              = make();
    h.filters().text    = "pkg:b";
    h.request_rebuild();
    h.tick();
    CHECK_EQ(count_nodes(h), 1);
}

// "Show me only what the agent touched plus affected context" (FR-35).
TEST(hiding_unaffected_nodes_keeps_changed_and_impacted_only) {
    auto h = make();
    push_change(h.store(), "a/x.ts", "file:a/x.ts");
    push_impact(h.store(), Level::Package,
                {ImpactedNode{"pkg:a", 0, false, true, Freshness::Current,
                              ImpactCause::Implementation, {}, false},
                 ImpactedNode{"pkg:b", 1, true, false, Freshness::Current,
                              ImpactCause::Implementation, {}, false}});
    h.filters().show_unaffected = false;
    h.request_rebuild();
    h.tick();

    CHECK(h.node("pkg:a") != entt::null);
    CHECK(h.node("pkg:b") != entt::null);
    CHECK(h.node("pkg:c") == entt::null);   // untouched, unimpacted
}

// -- impact state -------------------------------------------------------------

// A changed file must make its owning package read as changed, or the architecture
// view shows nothing at all while the agent is working.
TEST(a_changed_file_marks_its_owning_package_as_changed) {
    auto h = make();
    push_change(h.store(), "a/x.ts", "file:a/x.ts");
    h.tick();

    CHECK(h.registry().all_of<ecs::Changed>(h.node("pkg:a")));
    CHECK(!h.registry().all_of<ecs::Changed>(h.node("pkg:b")));
    CHECK_EQ(h.stats().changed, 1);
}

// THE rule. The impact result reports how trustworthy the PATH is, which can be worse
// than anything the node says about itself. Styling from node freshness alone renders
// a stale conclusion as current, which NFR-04 forbids.
TEST(a_stale_impact_conclusion_is_styled_as_stale_even_when_the_node_is_current) {
    auto h = make();
    CHECK(h.store().node("pkg:b")->freshness == Freshness::Current);

    push_impact(h.store(), Level::Package,
                {ImpactedNode{"pkg:b", 1, true, false, Freshness::Stale,
                              ImpactCause::Implementation, {}, false}});
    h.tick();

    const auto& style = h.registry().get<ecs::Style>(h.node("pkg:b"));
    // A dashed outline in the stale colour: two channels, because colour alone is not
    // enough to stop someone trusting the result.
    CHECK(style.dash > 0.0f);
    CHECK_EQ(h.stats().stale, 1);
}

TEST(a_current_impact_conclusion_is_not_marked_uncertain) {
    auto h = make();
    push_impact(h.store(), Level::Package,
                {ImpactedNode{"pkg:b", 1, true, false, Freshness::Current,
                              ImpactCause::Implementation, {}, false}});
    h.tick();

    CHECK_EQ(h.registry().get<ecs::Style>(h.node("pkg:b")).dash, 0.0f);
    CHECK_EQ(h.stats().stale, 0);
}

// -- selection: the divergence this refactor exists to make impossible --------

// Selection used to live in two places kept in step by hand, and one call site set the
// id without the component, so selecting from the inspector drew no outline. Now the
// component is derived, so the only way to select is a command and both always agree.
TEST(a_select_command_updates_both_the_id_and_the_component) {
    auto h = make();
    h.commands().push(ecs::SelectNode{"pkg:b"});
    h.tick();

    CHECK_EQ(h.selection().node, std::string("pkg:b"));
    CHECK(h.registry().all_of<ecs::Selected>(h.node("pkg:b")));
    CHECK(!h.registry().all_of<ecs::Selected>(h.node("pkg:a")));
}

TEST(clearing_the_selection_removes_the_component) {
    auto h = make();
    h.commands().push(ecs::SelectNode{"pkg:b"});
    h.tick();
    h.commands().push(ecs::ClearSelection{});
    h.tick();

    CHECK(h.selection().node.empty());
    int selected = 0;
    for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::Selected>().each()) ++selected;
    CHECK_EQ(selected, 0);
}

// Selecting a node that was deleted by an event must not resurrect or crash: the id is
// kept, because the disappearance is itself information, but nothing is marked.
TEST(selecting_a_node_that_no_longer_exists_marks_nothing) {
    auto h = make();
    h.commands().push(ecs::SelectNode{"pkg:does-not-exist"});
    h.tick();

    CHECK_EQ(h.selection().node, std::string("pkg:does-not-exist"));
    int selected = 0;
    for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::Selected>().each()) ++selected;
    CHECK_EQ(selected, 0);
}

// Selecting an impacted node lights the chain that explains it. If the hop marks are
// wrong the canvas highlights a path the inspector is not describing.
TEST(the_explained_path_marks_every_node_and_edge_on_the_chain) {
    auto h = make();
    push_impact(h.store(), Level::Package,
                {ImpactedNode{"pkg:c", 2, false, false, Freshness::Current,
                              ImpactCause::Implementation,
                              {ImpactPath{{"e:c->b", "e:b->a"}}}, false}});
    h.commands().push(ecs::SelectNode{"pkg:c"});
    h.tick();

    auto on_path = [&](const std::string& id, bool is_edge) {
        const entt::entity e = is_edge ? h.edge(id) : h.node(id);
        return e != entt::null && h.registry().all_of<ecs::OnExplainedPath>(e);
    };
    CHECK(on_path("pkg:c", false));
    CHECK(on_path("e:c->b", true));
    CHECK(on_path("pkg:b", false));
    CHECK(on_path("e:b->a", true));
    CHECK(on_path("pkg:a", false));
}

TEST(clearing_the_selection_clears_the_explained_path) {
    auto h = make();
    push_impact(h.store(), Level::Package,
                {ImpactedNode{"pkg:c", 2, false, false, Freshness::Current,
                              ImpactCause::Implementation,
                              {ImpactPath{{"e:c->b", "e:b->a"}}}, false}});
    h.commands().push(ecs::SelectNode{"pkg:c"});
    h.tick();
    h.commands().push(ecs::ClearSelection{});
    h.tick();

    int marked = 0;
    for ([[maybe_unused]] auto&& row :
         h.registry().view<const ecs::OnExplainedPath>().each()) {
        ++marked;
    }
    CHECK_EQ(marked, 0);
}

// CyclePath just adds a delta; the system that knows how many explanations exist is
// the one that wraps it.
TEST(cycling_past_the_last_path_wraps_to_the_first) {
    auto h = make();
    ImpactedNode n{"pkg:c", 2, false, false, Freshness::Current, ImpactCause::Implementation,
                   {ImpactPath{{"e:c->b", "e:b->a"}}, ImpactPath{{"e:c->b"}}}, false};
    push_impact(h.store(), Level::Package, {n});
    h.commands().push(ecs::SelectNode{"pkg:c"});
    h.tick();

    h.commands().push(ecs::CyclePath{1});
    h.tick();
    CHECK_EQ(h.selection().path_index, 1);

    h.commands().push(ecs::CyclePath{1});
    h.tick();
    CHECK_EQ(h.selection().path_index, 0);

    h.commands().push(ecs::CyclePath{-1});
    h.tick();
    CHECK_EQ(h.selection().path_index, 1);
}

// -- layout -------------------------------------------------------------------

// Depth 0 = depends on nothing in view, and rows go upward from there, so a blast
// radius reads bottom-to-top the way the spec draws it.
TEST(layout_puts_dependencies_below_their_dependents) {
    auto h = make();
    h.settle();

    auto depth_of = [&](const char* id) {
        return h.registry().get<ecs::Depth>(h.node(id)).value;
    };
    CHECK_EQ(depth_of("pkg:a"), 0);
    CHECK_EQ(depth_of("pkg:b"), 1);
    CHECK_EQ(depth_of("pkg:c"), 2);

    auto y_of = [&](const char* id) {
        return h.registry().get<ecs::LayoutTarget>(h.node(id)).p.y;
    };
    CHECK(y_of("pkg:c") < y_of("pkg:b"));   // screen y grows downward
    CHECK(y_of("pkg:b") < y_of("pkg:a"));
}

// Import graphs really do cycle. Layout must terminate and stay finite.
TEST(layout_survives_a_dependency_cycle) {
    Snapshot s = chain();
    s.edges.push_back(mk_edge("e:a->c", EdgeKind::DependsOn, "pkg:a", "pkg:c"));
    auto h = make(s);
    h.settle();

    for (auto [e, ref, d] : h.registry().view<const ecs::NodeRef, const ecs::Depth>().each()) {
        CHECK(d.value >= 0);
        CHECK(d.value < 64);
    }
}

// Layout must not throw away positions when the graph changes, or every file save
// reshuffles the screen (spec 11.2).
TEST(adding_a_node_does_not_move_the_existing_ones) {
    auto h = make();
    h.settle();
    const Vec2 before = h.registry().get<ecs::Position>(h.node("pkg:c")).p;

    GraphUpdatedPayload p;
    p.added_nodes = {mk_node("pkg:d", NodeKind::Package, "repo")};
    p.added_edges = {mk_edge("e:d->a", EdgeKind::DependsOn, "pkg:d", "pkg:a")};
    Event ev;
    ev.type       = EventType::GraphUpdated;
    ev.generation = 102;
    ev.payload    = p;
    h.store().on_event(ev);
    h.settle();

    const Vec2 after = h.registry().get<ecs::Position>(h.node("pkg:c")).p;
    CHECK(std::abs(after.y - before.y) < 1.0f);   // same row
    CHECK(h.node("pkg:d") != entt::null);
}

TEST(a_pinned_node_is_left_alone_by_layout) {
    auto h = make();
    h.settle();

    const entt::entity e = h.node("pkg:b");
    h.registry().emplace<ecs::Pinned>(e);
    h.registry().get<ecs::Position>(e).p = Vec2{999.0f, -999.0f};
    h.tick(1.0f / 60.0f, 60);

    const Vec2 p = h.registry().get<ecs::Position>(e).p;
    CHECK_EQ(p.x, 999.0f);
    CHECK_EQ(p.y, -999.0f);
}

// -- semantic zoom ------------------------------------------------------------

// Below the point where a label is readable, a labelled box is a smear. The node
// becomes a dot and the hover card takes over naming it.
TEST(labels_switch_off_once_they_would_be_illegible) {
    CHECK(view::node_detail(1.0f, 1.0f).labels);
    CHECK_EQ(view::node_detail(1.0f, 1.0f).t, 1.0f);

    CHECK(!view::node_detail(0.15f, 1.0f).labels);   // 13px * 0.15 = ~2px of text
    CHECK_EQ(view::node_detail(0.15f, 1.0f).t, 0.0f);
}

// A bigger text preference keeps labels alive further out, because they really are
// still readable there.
TEST(a_larger_text_scale_keeps_labels_readable_at_lower_zoom) {
    CHECK(view::node_detail(0.45f, 2.0f).t > view::node_detail(0.45f, 1.0f).t);
}

// The collapsed dot holds a constant SCREEN size. If it scaled with the world it would
// vanish at overview zoom, which is exactly where it is needed.
TEST(a_collapsed_node_holds_a_constant_screen_size) {
    const Vec2 layout_half{120.0f, 21.0f};
    auto       screen_h = [&](float zoom) {
        return view::render_half(zoom, view::node_detail(zoom, 1.0f), layout_half, 6.0f).y * zoom;
    };
    CHECK(std::abs(screen_h(0.10f) - screen_h(0.05f)) < 0.01f);
    CHECK(std::abs(screen_h(0.10f) - 6.0f) < 0.01f);
}

TEST(a_fully_zoomed_in_node_is_drawn_at_its_layout_footprint) {
    const Vec2 layout_half{120.0f, 21.0f};
    const Vec2 drawn = view::render_half(2.0f, view::node_detail(2.0f, 1.0f), layout_half, 6.0f);
    CHECK(std::abs(drawn.x - layout_half.x) < 0.01f);
    CHECK(std::abs(drawn.y - layout_half.y) < 0.01f);
}

// What the eye should be drawn to is bigger at overview scale, where colour and size
// are the only channels left.
TEST(changed_and_impacted_nodes_get_larger_dots_than_context) {
    const float context  = view::dot_px_for(false, false, false);
    const float impacted = view::dot_px_for(false, true, false);
    const float changed  = view::dot_px_for(true, true, false);
    CHECK(impacted > context);
    CHECK(changed > impacted);
    CHECK(view::dot_px_for(false, false, true) > context);   // selected or hovered
}

// Layout must not reflow when the user zooms: the drawn size collapses, the footprint
// layout reserved does not.
TEST(zooming_does_not_change_the_layout_footprint) {
    auto h = make();
    h.settle();
    const Vec2 before = h.registry().get<ecs::Extent>(h.node("pkg:b")).half;

    h.camera().zoom = 0.05f;
    h.tick();
    CHECK_EQ(before.x, h.registry().get<ecs::Extent>(h.node("pkg:b")).half.x);
    CHECK_EQ(before.y, h.registry().get<ecs::Extent>(h.node("pkg:b")).half.y);
}

// Node boxes are sized to hold their label, so the text preference changes geometry --
// and re-laying out must keep nodes in their rows rather than reshuffling the graph.
TEST(graph_text_scale_resizes_node_boxes_without_losing_positions) {
    auto h = make();
    h.settle();
    const entt::entity e      = h.node("pkg:b");
    const Vec2         before = h.registry().get<ecs::Extent>(e).half;
    const float        row_y  = h.registry().get<ecs::Position>(e).p.y;

    h.view().graph_text_scale                                = 2.0f;
    h.world.resource<ecs::SceneRequests>().refresh_extents    = true;
    h.settle();

    const Vec2 after = h.registry().get<ecs::Extent>(e).half;
    CHECK(after.x > before.x);
    CHECK(after.y > before.y);
    CHECK(std::abs(h.registry().get<ecs::Position>(e).p.y - row_y) < 1.0f);
}

// -- picking ------------------------------------------------------------------

TEST(picking_agrees_with_the_camera_transform) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    const entt::entity target = h.node("pkg:b");
    const Vec2         centre = h.registry().get<ecs::Position>(target).p;

    h.point_at(h.camera().world_to_screen(centre));
    CHECK(h.pointer().entity == target);

    h.point_at(h.camera().world_to_screen(centre + Vec2{9000.0f, 9000.0f}));
    CHECK(h.pointer().entity == entt::null);
}

// Clicks have to land on what is drawn. A dot only a few pixels across still needs to
// be hittable, or overview zoom becomes a test of mouse precision.
TEST(a_collapsed_dot_is_still_clickable) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});
    h.camera().zoom = 0.06f;
    CHECK(!view::node_detail(h.camera().zoom, h.view().graph_text_scale).labels);

    const entt::entity target = h.node("pkg:b");
    const Vec2         centre = h.registry().get<ecs::Position>(target).p;

    h.point_at(h.camera().world_to_screen(centre));
    CHECK(h.pointer().entity == target);

    // A few pixels off-centre still hits, because the hit area has a screen floor.
    h.point_at(h.camera().world_to_screen(centre) + Vec2{4.0f, 4.0f});
    CHECK(h.pointer().entity == target);
}

// Pressing on a node asks for it to be selected; the command is what makes it so.
TEST(clicking_a_node_selects_it_through_a_command) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    const entt::entity target = h.node("pkg:b");
    const Vec2         centre = h.registry().get<ecs::Position>(target).p;

    h.point_at(h.camera().world_to_screen(centre));   // hover first
    h.point_at(h.camera().world_to_screen(centre), /*press=*/true);
    h.tick();   // the command lands on the next frame

    CHECK_EQ(h.selection().node, std::string("pkg:b"));
    CHECK(h.registry().all_of<ecs::Selected>(target));
}

// -- navigation ---------------------------------------------------------------

// Regression: the wheel used to be read from ImGui's io, which zeroes it in
// EndFrame() -- so by the time the next frame's Input phase looked, it was always 0
// and zoom was silently dead. WindowSystem now owns the accumulator.
TEST(scrolling_zooms_the_camera) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    const float before = h.camera().zoom;
    h.input().mouse    = Vec2{600.0f, 400.0f};   // inside the free rect
    h.input().wheel    = 1.0f;
    h.tick();

    CHECK(h.camera().zoom > before);
}

TEST(scrolling_down_zooms_out) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    const float before = h.camera().zoom;
    h.input().mouse    = Vec2{600.0f, 400.0f};
    h.input().wheel    = -1.0f;
    h.tick();

    CHECK(h.camera().zoom < before);
}

// The thing under the pointer must stay under the pointer, or zooming feels like the
// graph is sliding away from you.
TEST(zooming_keeps_the_point_under_the_cursor_fixed) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    const Vec2 cursor = Vec2{700.0f, 300.0f};
    const Vec2 before = h.camera().screen_to_world(cursor);

    h.input().mouse = cursor;
    h.input().wheel = 2.0f;
    h.tick();

    const Vec2 after = h.camera().screen_to_world(cursor);
    CHECK(std::abs(after.x - before.x) < 0.5f);
    CHECK(std::abs(after.y - before.y) < 0.5f);
}

// Once the user has placed the view, layout stops moving it.
TEST(scrolling_takes_the_camera_off_auto_fit) {
    auto h = make();
    CHECK(h.world.resource<ecs::CameraControl>().auto_fit);

    h.input().mouse = Vec2{600.0f, 400.0f};
    h.input().wheel = 1.0f;
    h.tick();

    CHECK(!h.world.resource<ecs::CameraControl>().auto_fit);
}

// A panel that wants the wheel must get it; otherwise scrolling the event log also
// zooms the graph behind it.
TEST(scrolling_over_a_panel_does_not_zoom_the_graph) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    const float before        = h.camera().zoom;
    h.input().mouse           = Vec2{600.0f, 400.0f};
    h.input().wheel           = 1.0f;
    h.input().ui_wants_mouse  = true;
    h.tick();

    CHECK_EQ(h.camera().zoom, before);
}

// -- the radial filesystem layout --------------------------------------------

namespace {

// The filesystem view, laid out and settled.
rgvtest::Harness make_filesystem() {
    rgvtest::Harness h;
    h.store().reset(chain());
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();
    return h;
}

} // namespace

// Discs are how the view says "this is laid out radially". They must not leak into the
// box-based views, or picking and rendering would use the wrong shape there.
TEST(discs_exist_only_in_the_filesystem_view) {
    auto h = make_filesystem();
    int  discs = 0;
    for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::Disc>().each()) ++discs;
    CHECK(discs > 0);

    h.view().mode = ecs::ViewMode::Architecture;
    h.request_rebuild();
    h.settle();
    discs = 0;
    for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::Disc>().each()) ++discs;
    CHECK_EQ(discs, 0);
}

// The repository anchors the tree. Without it every package is a root and the layout
// becomes a ring with a hole in the middle.
TEST(the_filesystem_view_grows_from_the_repository) {
    auto h = make_filesystem();
    const entt::entity repo = h.node("repo");
    CHECK(repo != entt::null);

    const Vec2 at = h.registry().get<ecs::LayoutTarget>(repo).p;
    CHECK(std::abs(at.x) < 0.01f);
    CHECK(std::abs(at.y) < 0.01f);
}

// A directory's radius is its file count made visible -- the reason the view reads at
// a glance without labels.
TEST(a_directory_disc_grows_with_the_files_it_holds) {
    Snapshot s = chain();
    for (int i = 0; i < 20; ++i) {
        s.nodes.push_back(mk_node("file:a/extra" + std::to_string(i) + ".ts", NodeKind::File,
                                  "dir:a"));
    }
    rgvtest::Harness h;
    h.store().reset(s);
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();

    // dir:a now holds 21 files; pkg:b holds one and should stay at the floor.
    CHECK(h.registry().get<ecs::Disc>(h.node("dir:a")).radius >
          h.registry().get<ecs::Disc>(h.node("pkg:b")).radius);
}

// Files orbit the directory that owns them, clear of it.
//
// They used to be placed at exactly the drawn radius, so every file dot straddled its
// directory's edge and half-occluded it. The drawn disc and the orbit are separate
// things now, and this is the invariant that keeps them apart.
TEST(files_orbit_their_directory_without_touching_it) {
    auto h = make_filesystem();

    const Vec2  dir       = h.registry().get<ecs::LayoutTarget>(h.node("dir:a")).p;
    const float dir_r     = h.registry().get<ecs::Disc>(h.node("dir:a")).radius;
    const Vec2  file      = h.registry().get<ecs::LayoutTarget>(h.node("file:a/x.ts")).p;
    const float file_r    = h.registry().get<ecs::Disc>(h.node("file:a/x.ts")).radius;

    CHECK(length(file - dir) > dir_r + file_r);
}

// Files of one directory share a single orbit, so the halo reads as a ring.
TEST(files_of_one_directory_share_an_orbit) {
    Snapshot s = chain();
    for (int i = 0; i < 6; ++i) {
        s.nodes.push_back(mk_node("file:a/f" + std::to_string(i) + ".ts", NodeKind::File,
                                  "dir:a"));
    }
    rgvtest::Harness h;
    h.store().reset(s);
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();

    const Vec2 dir = h.registry().get<ecs::LayoutTarget>(h.node("dir:a")).p;
    float      first = -1.0f;
    for (int i = 0; i < 6; ++i) {
        const entt::entity e = h.node("file:a/f" + std::to_string(i) + ".ts");
        const float d = length(h.registry().get<ecs::LayoutTarget>(e).p - dir);
        if (first < 0.0f) first = d;
        else CHECK(std::abs(d - first) < 0.5f);
    }
}

// A file is smaller than a directory, but recognisably the same kind of thing. The
// ratio used to be a dot against a whole orbit, which read as two unrelated shapes.
TEST(a_file_is_smaller_than_a_directory_but_not_dramatically) {
    auto h = make_filesystem();

    const float file = h.registry().get<ecs::Disc>(h.node("file:a/x.ts")).radius;
    const float dir  = h.registry().get<ecs::Disc>(h.node("dir:a")).radius;

    CHECK(dir > file);
    CHECK(dir < file * 3.0f);
}

// The claim the whole layout rests on: collisions are prevented by construction, not
// by relaxation. Every subtree is laid out in its own frame first, so its enclosing
// radius is exact rather than estimated, and a parent packs those as rigid discs.
// Nothing overlaps at all -- not even a file against the directory that owns it.
TEST(no_two_discs_overlap) {
    Snapshot s = chain();
    // A lopsided tree: one fat directory and one deep chain, so the packing is tested
    // rather than a symmetric best case.
    for (int i = 0; i < 14; ++i) {
        s.nodes.push_back(mk_node("file:a/f" + std::to_string(i) + ".ts", NodeKind::File,
                                  "dir:a"));
    }
    s.nodes.push_back(mk_node("dir:b/deep", NodeKind::Directory, "pkg:b"));
    s.nodes.push_back(mk_node("dir:b/deep/deeper", NodeKind::Directory, "dir:b/deep"));
    s.nodes.push_back(mk_node("file:b/deep/deeper/z.ts", NodeKind::File, "dir:b/deep/deeper"));

    rgvtest::Harness h;
    h.store().reset(s);
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();

    std::vector<std::tuple<entt::entity, Vec2, float>> discs;
    for (auto [e, t, d] : h.registry().view<const ecs::LayoutTarget, const ecs::Disc>().each()) {
        discs.emplace_back(e, t.p, d.radius);
    }
    CHECK(discs.size() > 10);

    int overlaps = 0;
    for (std::size_t i = 0; i < discs.size(); ++i) {
        for (std::size_t j = i + 1; j < discs.size(); ++j) {
            const auto [ea, pa, ra] = discs[i];
            const auto [eb, pb, rb] = discs[j];
            if (length(pa - pb) + 0.5f < ra + rb) ++overlaps;
        }
    }
    CHECK_EQ(overlaps, 0);
}
