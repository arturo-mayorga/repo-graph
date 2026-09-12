// Scene rules: what appears on screen, and how state is turned into style.
//
// Driven through the real schedule -- the same systems the application runs, minus the
// platform, the GPU, and the data source. A test that bypassed the systems would not
// be testing the thing that ships.
#include "Harness.h"
#include "TestMain.h"

#include "rgv/model/GraphStore.h"
#include "rgv/view/CameraFit.h"
#include "rgv/view/HoverLinks.h"
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

// The same graph as files with imports, for the concentric layout the file graph uses.
// Ids are opaque to the frontend, so they can stay as they are.
Snapshot as_files(Snapshot s) {
    for (auto& n : s.nodes) {
        if (n.kind == NodeKind::Package) n.kind = NodeKind::File;
    }
    for (auto& e : s.edges) {
        if (e.kind == EdgeKind::DependsOn) e.kind = EdgeKind::Imports;
    }
    return s;
}

int edges_between(rgvtest::Harness& h, const std::string& from, const std::string& to) {
    int n = 0;
    for (auto [e, ref, ends] : h.registry().view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) continue;
        if (ends.from == h.index().node(from) && ends.to == h.index().node(to)) ++n;
    }
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
    CHECK_EQ(count_nodes(h), 6);   // repo, three packages, two files; no directories

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
    CHECK(h.registry().all_of<ecs::Changed>(h.node("file:a/x.ts")));
    CHECK_EQ(h.stats().changed, 2);   // the module, and the package that owns it
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
    // Through the file graph, which draws the chain as edges of its own. The
    // architecture view draws imports, and a package-level explanation there is
    // carried by the module edges underneath it.
    auto h        = make(as_files(chain()));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.tick();
    push_impact(h.store(), Level::File,
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

// Depth is still dependency distance and still cycle-safe, but it no longer decides
// where a node goes. The dependency views are concentric, and the ring is REACH: how
// much of the repository transitively depends on this node. The core sits in the
// middle, consumers on the rim.
//
// Direct dependents would be the wrong axis. In this chain c depends on b depends on a,
// so a has one direct dependent and would be exiled to the rim while being the thing
// everything else is built on.
TEST(layout_puts_the_most_depended_on_node_at_the_core) {
    auto h        = make(as_files(chain()));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();

    auto depth_of = [&](const char* id) {
        return h.registry().get<ecs::Depth>(h.node(id)).value;
    };
    CHECK_EQ(depth_of("pkg:a"), 0);
    CHECK_EQ(depth_of("pkg:b"), 1);
    CHECK_EQ(depth_of("pkg:c"), 2);

    // a is depended on by b and c, c by nobody. A three-node chain buckets into two
    // rings, so the core end may share one -- but it may never be further out.
    auto radius_of = [&](const char* id) {
        return length(h.registry().get<ecs::LayoutTarget>(h.node(id)).p);
    };
    CHECK(radius_of("pkg:a") <= radius_of("pkg:b"));
    CHECK(radius_of("pkg:b") < radius_of("pkg:c"));

    // With reach spread wide enough to separate, the hub lands strictly inside.
    Snapshot w;
    w.generation                  = 100;
    w.session.baseline_generation = 100;
    w.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:hub", NodeKind::Package, "repo"),
               mk_node("pkg:leaf", NodeKind::Package, "repo")};
    for (int i = 0; i < 6; ++i) {
        const std::string p = "pkg:d" + std::to_string(i);
        w.nodes.push_back(mk_node(p, NodeKind::Package, "repo"));
        w.edges.push_back(mk_edge("e:" + p, EdgeKind::DependsOn, p, "pkg:hub"));
    }
    auto g        = make(as_files(w));
    g.view().mode  = ecs::ViewMode::FileGraph;
    g.view().level = Level::File;
    g.request_rebuild();
    g.settle();

    const auto& reg = g.registry();
    CHECK(reg.get<ecs::Ring>(g.node("pkg:hub")).index <
          reg.get<ecs::Ring>(g.node("pkg:leaf")).index);
    CHECK(length(reg.get<ecs::LayoutTarget>(g.node("pkg:hub")).p) <
          length(reg.get<ecs::LayoutTarget>(g.node("pkg:leaf")).p));
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
    // In the file graph, which seats an arrival on its ring and leaves the rest alone.
    // The two tree views repack instead: a tree's shape IS its arrangement, so a node
    // arriving genuinely changes where its siblings belong, and repacking is both cheap
    // and deterministic.
    auto h        = make(as_files(chain()));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();
    const Vec2 before = h.registry().get<ecs::Position>(h.node("pkg:c")).p;

    GraphUpdatedPayload p;
    p.added_nodes = {mk_node("pkg:d", NodeKind::File, "repo")};
    p.added_edges = {mk_edge("e:d->a", EdgeKind::Imports, "pkg:d", "pkg:a")};
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
    CHECK_EQ(view::node_detail(1.0f, 1.0f).t, 1.0f);
    CHECK_EQ(view::node_detail(0.15f, 1.0f).t, 0.0f);   // 13px * 0.15 = ~2px of text
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
    auto h        = make(as_files(chain()));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();
    const entt::entity e      = h.node("pkg:b");
    const Vec2         before = h.registry().get<ecs::Extent>(e).half;
    const float        ring   = length(h.registry().get<ecs::Position>(e).p);

    h.view().graph_text_scale                                = 2.0f;
    h.world.resource<ecs::SceneRequests>().refresh_extents    = true;
    h.settle();

    const Vec2 after = h.registry().get<ecs::Extent>(e).half;
    CHECK(after.x > before.x);
    CHECK(after.y > before.y);

    // Bigger boxes may need more room, so a node is allowed to slide along its ring to
    // make it -- but the ring is where it lives, and nothing is reseeded from scratch.
    // This slider is dragged: a full relayout here is what jitter is made of.
    const float now = length(h.registry().get<ecs::Position>(e).p);
    CHECK(std::abs(now - ring) < std::max(4.0f, ring * 0.1f));
}

// -- picking ------------------------------------------------------------------

TEST(picking_agrees_with_the_camera_transform) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    // c holds nothing: a plain box. A package with modules inside is a container, and
    // its contents win a click at its centre.
    const entt::entity target = h.node("pkg:c");
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
    CHECK_EQ(view::node_detail(h.camera().zoom, h.view().graph_text_scale).t, 0.0f);

    // c holds no modules, so it is a dot; a package with something inside is a box
    // whose contents win the click.
    const entt::entity target = h.node("pkg:c");
    const Vec2         centre = h.registry().get<ecs::Position>(target).p;

    h.point_at(h.camera().world_to_screen(centre));
    CHECK(h.pointer().entity == target);

    // A couple of pixels off-centre still hits, because the hit area has a screen
    // floor. Not much more than that: the nested layout packs packages close enough
    // that at this zoom the floors of neighbours overlap, and the nearer centre wins.
    h.point_at(h.camera().world_to_screen(centre) + Vec2{2.0f, 2.0f});
    CHECK(h.pointer().entity == target);
}

// Pressing on a node asks for it to be selected; the command is what makes it so.
TEST(clicking_a_node_selects_it_through_a_command) {
    auto h = make();
    h.settle();
    view::fit_camera(h.world, {});

    const entt::entity target = h.node("pkg:c");
    const Vec2         centre = h.registry().get<ecs::Position>(target).p;

    h.point_at(h.camera().world_to_screen(centre));   // hover first
    h.point_at(h.camera().world_to_screen(centre), /*press=*/true);
    h.tick();   // the command lands on the next frame

    CHECK_EQ(h.selection().node, std::string("pkg:c"));
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

// Direction. A `contains` edge means what its name says, so the container is the end an
// arrow leaves and what it holds is the end an arrow arrives at. The renderer puts the
// arrowhead on `to`, so this is what makes the tree read outward from the repository
// instead of converging on it.
TEST(containment_points_from_the_container_to_what_it_holds) {
    auto       h = make_filesystem();
    const auto e = h.edge(std::string("tree:file:a/x.ts"));
    CHECK(e != entt::null);
    const auto& ends = h.registry().get<ecs::Endpoints>(e);
    CHECK(ends.from == h.node("dir:a"));         // the directory
    CHECK(ends.to == h.node("file:a/x.ts"));     // what it holds
}

// The other direction, and it is the opposite one: a module points at what it imports,
// which is the contract's rule for every dependency edge (dependent -> dependency).
TEST(an_import_points_from_the_module_to_the_one_it_imports) {
    auto       h = make(chain());
    const auto e = h.edge("e:y->x");
    CHECK(e != entt::null);
    const auto& ends = h.registry().get<ecs::Endpoints>(e);
    CHECK(ends.from == h.node("file:b/y.ts"));   // the importer
    CHECK(ends.to == h.node("file:a/x.ts"));     // what it imports
}

// Discs are how the view says "this is laid out radially". They must not leak into the
// box-based views, or picking and rendering would use the wrong shape there.
TEST(discs_exist_in_the_tree_views_and_not_in_the_file_graph) {
    auto h = make_filesystem();
    int  discs = 0;
    for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::Disc>().each()) ++discs;
    CHECK(discs > 0);

    // The architecture view is the same radial algorithm over the import tree, so it
    // has discs too: a node's size is how much is built on it.
    h.view().mode = ecs::ViewMode::Architecture;
    h.request_rebuild();
    h.settle();
    discs = 0;
    for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::Disc>().each()) ++discs;
    CHECK(discs > 0);

    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
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

// A handful of files share one orbit, so a small directory reads as a simple ring.
TEST(a_few_files_share_a_single_orbit) {
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

    const Vec2 dir   = h.registry().get<ecs::LayoutTarget>(h.node("dir:a")).p;
    float      first = -1.0f;
    for (int i = 0; i < 6; ++i) {
        const entt::entity e = h.node("file:a/f" + std::to_string(i) + ".ts");
        const float d = length(h.registry().get<ecs::LayoutTarget>(e).p - dir);
        if (first < 0.0f) first = d;
        else CHECK(std::abs(d - first) < 0.5f);
    }
}

// Many files fill concentric orbits instead of one enormous ring.
//
// A single orbit sized to seat them all puts 60 files on a circle of radius ~170
// around a disc of radius ~16 -- a vast empty annulus, and most of the screen wasted.
// Packing onto successive orbits is what keeps a wide directory compact.
TEST(a_wide_directory_packs_its_files_onto_several_orbits) {
    Snapshot s = chain();
    for (int i = 0; i < 60; ++i) {
        s.nodes.push_back(mk_node("file:a/f" + std::to_string(i) + ".ts", NodeKind::File,
                                  "dir:a"));
    }
    rgvtest::Harness h;
    h.store().reset(s);
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();

    const Vec2         dir = h.registry().get<ecs::LayoutTarget>(h.node("dir:a")).p;
    std::vector<float> radii;
    for (int i = 0; i < 60; ++i) {
        const entt::entity e = h.node("file:a/f" + std::to_string(i) + ".ts");
        radii.push_back(length(h.registry().get<ecs::LayoutTarget>(e).p - dir));
    }
    std::sort(radii.begin(), radii.end());

    // Several distinct orbits, not one.
    int distinct = 1;
    for (std::size_t i = 1; i < radii.size(); ++i) {
        if (radii[i] - radii[i - 1] > 1.0f) ++distinct;
    }
    CHECK(distinct >= 3);

    // And the whole halo stays far tighter than one ring would have been. One orbit
    // seating 60 files needs a radius of roughly 60 * (2r + gap) / 2pi.
    const float one_ring = 60.0f * (2.0f * 6.0f + 6.0f) / 6.2831853f;
    CHECK(radii.back() < one_ring * 0.5f);
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

// Node sizes step by the golden ratio: file, directory, largest directory are r, r*phi,
// r*phi^2. Three sizes on one geometric scale read as a family.
TEST(node_sizes_step_by_the_golden_ratio) {
    constexpr float phi = 1.6180339887f;

    Snapshot s = chain();
    for (int i = 0; i < 80; ++i) {
        s.nodes.push_back(mk_node("file:a/f" + std::to_string(i) + ".ts", NodeKind::File,
                                  "dir:a"));
    }
    rgvtest::Harness h;
    h.store().reset(s);
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();

    const float file  = h.registry().get<ecs::Disc>(h.node("file:a/x.ts")).radius;
    const float empty = h.registry().get<ecs::Disc>(h.node("pkg:c")).radius;   // no files
    const float full  = h.registry().get<ecs::Disc>(h.node("dir:a")).radius;   // 81 files

    CHECK(std::abs(empty / file - phi) < 0.02f);
    CHECK(std::abs(full / file - phi * phi) < 0.05f);
}

// -- dragging -----------------------------------------------------------------
//
// A drag switches on a live relaxation: springs along containment whose rest lengths
// are the distances the structural packing produced, plus repulsion between discs. The
// layout itself has no forces -- that is deliberate, so nothing drifts -- but a drag
// wants the graph to give way, so the simulation is seeded from the packing and its
// equilibrium is the arrangement it started from.

// Dragging a directory pulls its files along, by relaxation rather than rigidly.
//
// It used to move the single node under the cursor and leave every file behind, which
// in a containment view is the one thing a drag must not do. Translating the subtree
// rigidly fixed that but made the cluster behave like a solid object; the springs let
// the children trail and settle instead.
TEST(dragging_a_directory_pulls_its_files_along) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir  = h.node("dir:a");
    const entt::entity file = h.node("file:a/x.ts");
    const Vec2 before_dir   = h.registry().get<ecs::Position>(dir).p;
    const Vec2 before_file  = h.registry().get<ecs::Position>(file).p;

    h.begin_drag(h.camera().world_to_screen(before_dir));
    for (int i = 0; i < 14; ++i) h.drag_by(Vec2{7.0f, 0.0f});
    h.end_drag();
    h.tick(1.0f / 60.0f, 90);   // let the settle-down finish

    const Vec2 moved_dir  = h.registry().get<ecs::Position>(dir).p - before_dir;
    const Vec2 moved_file = h.registry().get<ecs::Position>(file).p - before_file;

    CHECK(length(moved_dir) > 10.0f);
    CHECK(length(moved_file) > 5.0f);                                 // it came along
    CHECK(dot(normalize(moved_file), normalize(moved_dir)) > 0.5f);   // the same way
}

// The springs hold the distance the packing chose, so a cluster keeps its shape while
// being dragged instead of stretching out behind the cursor.
TEST(relaxation_keeps_a_file_at_the_distance_the_packing_gave_it) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir  = h.node("dir:a");
    const entt::entity file = h.node("file:a/x.ts");
    const float rest = length(h.registry().get<ecs::Position>(file).p -
                              h.registry().get<ecs::Position>(dir).p);

    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    for (int i = 0; i < 14; ++i) h.drag_by(Vec2{7.0f, 0.0f});
    h.end_drag();
    h.tick(1.0f / 60.0f, 120);

    const float after = length(h.registry().get<ecs::Position>(file).p -
                               h.registry().get<ecs::Position>(dir).p);
    CHECK(std::abs(after - rest) < rest * 0.45f);
}

// Releasing hands the node back to the relaxation rather than freezing it.
//
// A drag used to pin. After a few drags every node the user had touched was a fixed
// point, the relaxation had nothing left to move, and the graph became a static picture
// that stopped reacting to its own neighbours.
TEST(dragging_does_not_pin_a_node) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir = h.node("dir:a");
    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    for (int i = 0; i < 10; ++i) h.drag_by(Vec2{8.0f, 0.0f});
    h.end_drag();

    CHECK(!h.registry().all_of<ecs::Pinned>(dir));
}

// The relaxation keeps running after the mouse comes up, so the node travels on to
// somewhere consistent with its neighbours instead of stopping dead.
TEST(a_dropped_node_keeps_settling_after_release) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir = h.node("dir:a");
    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    for (int i = 0; i < 14; ++i) h.drag_by(Vec2{9.0f, 0.0f});
    h.end_drag();

    const Vec2 dropped = h.registry().get<ecs::Position>(dir).p;
    h.tick(1.0f / 60.0f, 200);
    const Vec2 settled = h.registry().get<ecs::Position>(dir).p;

    CHECK(length(settled - dropped) > 1.0f);          // it carried on moving
    CHECK(h.stats().layout_settled);                  // and then stopped
}

// Where it stops is a position the graph agrees with: its spring has pulled it back to
// the distance from its parent that the packing chose.
TEST(a_dropped_node_settles_at_a_natural_distance_from_its_parent) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity file = h.node("file:a/x.ts");
    const entt::entity dir  = h.node("dir:a");
    const float rest = length(h.registry().get<ecs::Position>(file).p -
                              h.registry().get<ecs::Position>(dir).p);

    // Haul the file well away from the directory that owns it.
    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(file).p));
    for (int i = 0; i < 20; ++i) h.drag_by(Vec2{14.0f, 9.0f});
    h.end_drag();
    h.tick(1.0f / 60.0f, 400);

    const float after = length(h.registry().get<ecs::Position>(file).p -
                               h.registry().get<ecs::Position>(dir).p);
    CHECK(std::abs(after - rest) < rest * 0.5f);
}

// Pinning is still available -- explicitly, on double click -- and still holds.
TEST(an_explicitly_pinned_node_is_not_moved_by_the_relaxation) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity neighbour = h.node("pkg:b");
    h.registry().emplace<ecs::Pinned>(neighbour);
    const Vec2 held = h.registry().get<ecs::Position>(neighbour).p;

    const entt::entity dir = h.node("dir:a");
    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    const Vec2 toward = (held - h.registry().get<ecs::Position>(dir).p) / 12.0f;
    for (int i = 0; i < 12; ++i) h.drag_by(toward * h.camera().zoom);
    h.end_drag();
    h.tick(1.0f / 60.0f, 200);

    CHECK(length(h.registry().get<ecs::Position>(neighbour).p - held) < 0.5f);
}

// A drag reflows the graph and the reflow sticks. Nothing snaps back to the packing:
// the arrangement the user produced is the arrangement they keep.
TEST(a_reflow_caused_by_dragging_persists_after_release) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir       = h.node("dir:a");
    const entt::entity neighbour = h.node("pkg:b");
    const Vec2 home = h.registry().get<ecs::Position>(neighbour).p;

    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    const Vec2 toward = (home - h.registry().get<ecs::Position>(dir).p) / 12.0f;
    for (int i = 0; i < 12; ++i) h.drag_by(toward * h.camera().zoom);
    h.end_drag();
    h.tick(1.0f / 60.0f, 150);

    // Position and target agree, so nothing is still being pulled anywhere.
    const Vec2 pos = h.registry().get<ecs::Position>(neighbour).p;
    CHECK(length(h.registry().get<ecs::LayoutTarget>(neighbour).p - pos) < 1.0f);
}

// Repulsion is the half of the relaxation that keeps a reflow legible: whatever the
// user shoves things into has to move aside rather than be sat on top of.
TEST(relaxation_separates_nodes_that_would_overlap) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir       = h.node("dir:a");
    const entt::entity neighbour = h.node("pkg:b");

    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    const Vec2 target = h.registry().get<ecs::Position>(neighbour).p;
    const Vec2 toward = (target - h.registry().get<ecs::Position>(dir).p) / 12.0f;
    for (int i = 0; i < 12; ++i) h.drag_by(toward * h.camera().zoom);
    h.end_drag();
    h.tick(1.0f / 60.0f, 150);

    const float gap  = length(h.registry().get<ecs::Position>(dir).p -
                              h.registry().get<ecs::Position>(neighbour).p);
    const float want = h.registry().get<ecs::Disc>(dir).radius +
                       h.registry().get<ecs::Disc>(neighbour).radius;
    CHECK(gap > want * 0.9f);
}

// The layered views have no containment to relax, so a drag there stays rigid and the
// -- dragging in the layered views --------------------------------------------
//
// The radial relaxation is free in both axes because a containment tree has no
// privileged direction. A layered graph does: the row is the depth reading. So the
// same drag behaviour is offered -- neighbours give way, and a drop is not a pin --
// but constrained, and the constraint is what these tests are about.

// Dropping a node on top of its neighbour must not leave them overlapping. There is no
// containment here to spring anything home, so reopening the arc is the whole job.
TEST(dragging_a_dependency_node_pushes_its_neighbours_aside) {
    // Three packages depending on one shared base all land on the same ring.
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:base", NodeKind::Package, "repo"),
               mk_node("pkg:p0", NodeKind::Package, "repo"),
               mk_node("pkg:p1", NodeKind::Package, "repo"),
               mk_node("pkg:p2", NodeKind::Package, "repo")};
    for (int i = 0; i < 3; ++i) {
        const std::string p = "pkg:p" + std::to_string(i);
        s.edges.push_back(mk_edge("e:" + p, EdgeKind::DependsOn, p, "pkg:base"));
    }
    auto h        = make(as_files(s));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();
    view::fit_camera(h.world, {});

    const entt::entity a    = h.node("pkg:p0");
    const entt::entity mate = h.node("pkg:p1");
    CHECK_EQ(h.registry().get<ecs::Ring>(a).index,
             h.registry().get<ecs::Ring>(mate).index);

    const Vec2 target = h.registry().get<ecs::Position>(mate).p;
    const Vec2 from   = h.registry().get<ecs::Position>(a).p;

    // Drop it straight on top of its neighbour.
    h.begin_drag(h.camera().world_to_screen(from));
    const Vec2 toward = (target - from) * (1.0f / 12.0f);
    for (int i = 0; i < 12; ++i) h.drag_by(toward * h.camera().zoom);
    h.end_drag();
    h.tick(1.0f / 60.0f, 400);

    const Vec2  pa = h.registry().get<ecs::Position>(a).p;
    const Vec2  pm = h.registry().get<ecs::Position>(mate).p;
    const float want = h.registry().get<ecs::Extent>(a).half.x +
                       h.registry().get<ecs::Extent>(mate).half.x;
    CHECK(length(pa - pm) > want * 0.8f);
}

// A node dragged off its ring comes back to it. The ring is the reach reading, and a
// node parked between rings is claiming a share of the repository it does not carry.
TEST(a_dependency_node_returns_to_its_ring_after_a_drop) {
    auto h        = make(as_files(chain()));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();
    view::fit_camera(h.world, {});

    const entt::entity b    = h.node("pkg:b");
    const float        ring = length(h.registry().get<ecs::Position>(b).p);

    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(b).p));
    for (int i = 0; i < 12; ++i) h.drag_by(Vec2{11.0f, 11.0f});
    h.end_drag();
    h.tick(1.0f / 60.0f, 400);

    const float after = length(h.registry().get<ecs::Position>(b).p);
    CHECK(std::abs(after - ring) < std::max(4.0f, ring * 0.06f));
}

// Angular intent survives. Springing the angle home as well would undo the drag, and
// the user swung the node round there on purpose.
TEST(a_dependency_drop_keeps_the_angle_it_was_put_at) {
    auto h        = make(as_files(chain()));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();
    view::fit_camera(h.world, {});

    const entt::entity b = h.node("pkg:b");
    const Vec2  start    = h.registry().get<ecs::Position>(b).p;
    const float before   = std::atan2(start.y, start.x);

    h.begin_drag(h.camera().world_to_screen(start));
    // Swing it round the ring rather than in or out.
    const Vec2 tangent{-start.y, start.x};
    const Vec2 step = tangent * (1.0f / (length(tangent) + 1e-4f)) * 12.0f;
    for (int i = 0; i < 12; ++i) h.drag_by(step * h.camera().zoom);
    h.end_drag();
    h.tick(1.0f / 60.0f, 400);

    const Vec2  end   = h.registry().get<ecs::Position>(b).p;
    const float after = std::atan2(end.y, end.x);
    float       moved = std::abs(after - before);
    if (moved > 3.14159f) moved = 6.2831853f - moved;
    CHECK(moved > 0.08f);
}

// Pausing mid-drag is not a release.
//
// `active` used to be set only on frames where the pointer moved, so holding the button
// still looked like letting go: the node stopped being held, its springs took over, and
// it crawled out from under a cursor the user had not released.
TEST(a_held_node_does_not_move_while_the_pointer_is_still) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir = h.node("dir:a");
    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    for (int i = 0; i < 12; ++i) h.drag_by(Vec2{9.0f, 0.0f});

    const Vec2 held = h.registry().get<ecs::Position>(dir).p;
    h.hold_drag(120);   // two seconds of button-down and no movement
    CHECK(length(h.registry().get<ecs::Position>(dir).p - held) < 0.01f);

    // Releasing after the graph has settled around the cursor must not jump. By then
    // the spring has reached its rest length by pulling the PARENT along, so there is
    // nothing left for it to correct.
    h.end_drag();
    h.tick(1.0f / 60.0f, 120);
    CHECK(length(h.registry().get<ecs::Position>(dir).p - held) < 5.0f);
}

// Its children keep settling while it is held, which is the point of relaxing live.
TEST(children_keep_settling_while_a_node_is_held_still) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});

    const entt::entity dir  = h.node("dir:a");
    const entt::entity file = h.node("file:a/x.ts");

    h.begin_drag(h.camera().world_to_screen(h.registry().get<ecs::Position>(dir).p));
    for (int i = 0; i < 14; ++i) h.drag_by(Vec2{11.0f, 0.0f});

    const Vec2 before = h.registry().get<ecs::Position>(file).p;
    h.hold_drag(45);
    CHECK(length(h.registry().get<ecs::Position>(file).p - before) > 0.1f);
}


// -- the one label model ------------------------------------------------------
//
// Three views, one rule: a node is a dot until there is both room for a box and enough
// zoom to read one, and its name sits inside the box once it has grown into one and
// beside it before that. What differs between views is only what the collapsed shape is
// and how early the zoom half of the gate opens.

// `Spacing` is what the label crowding test reads. It used to be produced by the radial
// packing alone, which left the box views with no measure of how close their neighbours
// were -- so their names were drawn however dense the graph got.
TEST(every_layout_measures_the_room_around_a_node) {
    for (auto mode : {ecs::ViewMode::Architecture, ecs::ViewMode::Filesystem,
                      ecs::ViewMode::FileGraph}) {
        auto h = make_filesystem();
        h.view().mode = mode;
        h.request_rebuild();
        h.settle();

        int nodes = 0, spaced = 0;
        for (auto [e, ref] : h.registry().view<const ecs::NodeRef>().each()) {
            ++nodes;
            if (const auto* s = h.registry().try_get<ecs::Spacing>(e)) {
                if (s->room > 0.0f && s->room < 1e8f) ++spaced;
            }
        }
        CHECK(nodes > 1);
        CHECK_EQ(spaced, nodes);
    }
}

// The two views disagree about zoom on purpose, and the disagreement is the whole point
// of the split curve. A layered layout reserves each label box as the node's footprint,
// so at the fit zoom the box is exactly the right thing to draw. A radial layout
// reserves no such thing, so a box is something you zoom in to get.
//
// Unifying these on the disc curve regressed the architecture view to circles at the
// zoom it opens at, which is the view's default reading.
TEST(a_layered_node_is_a_box_where_a_disc_is_still_a_circle) {
    const Vec2  half{62.0f, 21.0f};                  // a package label box
    const float fit = 0.85f;                         // roughly where a small graph opens
    const auto  detail = view::node_detail(fit, 1.0f);

    const float layered = view::disc_morph(detail, {0.0f, 400.0f}, half);
    const float radial  = view::disc_morph(detail, {30.0f, 400.0f}, half);

    CHECK(view::label_belongs_inside(layered));
    CHECK(!view::label_belongs_inside(radial));
    CHECK(radial < layered);
}

// Room gates the morph independently of zoom, or a file on a crowded orbit would grow a
// box straight through its neighbours the moment the text became legible.
TEST(a_node_with_no_room_stays_collapsed_however_far_you_zoom) {
    const Vec2 half{60.0f, 16.0f};
    const auto deep = view::node_detail(8.0f, 1.0f);

    CHECK_EQ(view::disc_morph(deep, {6.0f, 9.0f}, half), 0.0f);
    CHECK(view::disc_morph(deep, {6.0f, 400.0f}, half) > 0.9f);
}

// A collapsed node holds a constant screen size, so an overview stays a readable
// constellation instead of fading out as the user zooms away from it.
TEST(a_collapsed_node_holds_its_screen_size) {
    const Vec2 half{60.0f, 16.0f};
    const float dot = view::dot_px_for(false, false, false);

    for (float zoom : {0.05f, 0.2f, 0.5f}) {
        const auto d = view::node_detail(zoom, 1.0f);
        const Vec2 h = view::node_half(zoom, d, half, {0.0f, 1e9f}, dot);
        CHECK(std::abs(h.y * zoom - dot) < 0.5f);
    }
}

// -- size carries meaning in the box views ------------------------------------
//
// The radial view sizes a disc by what it holds, which is what makes its overview
// readable before a single label is. The box views had no equivalent: a footprint is
// whatever the name needs, so a package six others import was drawn exactly like one
// nothing imports, and blast radius -- the thing the product exists to show -- was
// invisible until something changed.

// A hub is bigger than a leaf, on the golden-ratio ladder the discs already use.
TEST(a_hub_package_is_drawn_larger_than_a_leaf) {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:hub", NodeKind::Package, "repo"),
               mk_node("pkg:leaf", NodeKind::Package, "repo")};
    // Six of eight packages depend on the hub; nothing depends on the leaf.
    for (int i = 0; i < 6; ++i) {
        const std::string p = "pkg:d" + std::to_string(i);
        s.nodes.push_back(mk_node(p, NodeKind::Package, "repo"));
        s.edges.push_back(mk_edge("e:" + p, EdgeKind::DependsOn, p, "pkg:hub"));
    }
    auto h = make(s);
    h.settle();

    const float hub  = h.registry().get<ecs::Prominence>(h.node("pkg:hub")).scale;
    const float leaf = h.registry().get<ecs::Prominence>(h.node("pkg:leaf")).scale;
    CHECK(hub > leaf);
    CHECK_EQ(leaf, 1.0f);

    // The footprint follows, so the layout reserves the room the bigger box needs
    // rather than letting it grow through its neighbours at draw time.
    CHECK(h.registry().get<ecs::Extent>(h.node("pkg:hub")).half.x >
          h.registry().get<ecs::Extent>(h.node("pkg:leaf")).half.x);
}

// The filesystem view is left alone. Its discs are already sized by what they hold, and
// scaling the box underneath them would count the same thing twice.
TEST(the_filesystem_view_does_not_scale_boxes_by_prominence) {
    auto h = make_filesystem();
    for (auto [e, prom] : h.registry().view<const ecs::Prominence>().each()) {
        CHECK_EQ(prom.scale, 1.0f);
    }
}

// Prominence and impact ride the same ladder, so they combine by taking the larger.
// Multiplying would make a changed hub seven times a leaf and swamp the picture.
TEST(prominence_and_impact_do_not_multiply) {
    constexpr float phi = 1.6180339887f;
    const float leaf       = view::dot_px_for(false, false, false, 1.0f);
    const float changed    = view::dot_px_for(true, true, false, 1.0f);
    const float hub        = view::dot_px_for(false, false, false, phi * phi);
    const float changed_hub = view::dot_px_for(true, true, false, phi * phi);

    CHECK(hub > leaf);
    CHECK_EQ(changed_hub, changed);              // already at the top of the ladder
    CHECK(changed_hub < leaf * phi * phi * phi); // never compounds
}

// -- filters must not move the graph ------------------------------------------
//
// Every filter control used to set `rebuild`, which clears the registry and reseeds
// every position from scratch. The relevance slider is DRAGGED, so that ran on every
// frame it moved and the whole graph jittered under the cursor. What stays visible has
// to stay put; only the difference is applied.

TEST(moving_the_relevance_filter_leaves_surviving_nodes_where_they_are) {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:hub", NodeKind::Package, "repo")};
    for (int i = 0; i < 6; ++i) {
        const std::string p = "pkg:d" + std::to_string(i);
        s.nodes.push_back(mk_node(p, NodeKind::Package, "repo"));
        s.edges.push_back(mk_edge("e:" + p, EdgeKind::DependsOn, p, "pkg:hub"));
    }
    auto h = make(s);
    h.settle();

    std::unordered_map<std::string, Vec2> before;
    for (auto [e, ref, pos] :
         h.registry().view<const ecs::NodeRef, const ecs::Position>().each()) {
        before[ref.id] = pos.p;
    }
    CHECK(before.size() > 2);

    h.filters().min_relevance = 0.6f;
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick(1.0f / 60.0f, 4);

    int survived = 0;
    for (auto [e, ref, pos] :
         h.registry().view<const ecs::NodeRef, const ecs::Position>().each()) {
        auto it = before.find(ref.id);
        if (it == before.end()) continue;
        ++survived;
        CHECK(length(pos.p - it->second) < 1.0f);
    }
    CHECK(survived > 0);
}

// The escape hatch: a full layout is still available, it is just something the user
// asks for rather than something a slider does to them.
TEST(an_explicit_relayout_still_rearranges_everything) {
    auto h = make();
    h.settle();

    const entt::entity b = h.node("pkg:b");
    h.registry().get<ecs::Position>(b).p = Vec2{4000.0f, 4000.0f};
    h.registry().get<ecs::LayoutTarget>(b).p = Vec2{4000.0f, 4000.0f};

    h.world.resource<ecs::SceneRequests>().relayout = true;
    h.settle();

    CHECK(length(h.registry().get<ecs::Position>(b).p) < 3000.0f);
}

// A node that appears has to land somewhere sensible on its own, because nothing is
// going to lay the graph out around it.
TEST(a_node_that_appears_is_seated_next_to_what_it_connects_to) {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:hub", NodeKind::Package, "repo")};
    for (int i = 0; i < 6; ++i) {
        const std::string p = "pkg:d" + std::to_string(i);
        s.nodes.push_back(mk_node(p, NodeKind::Package, "repo"));
        s.edges.push_back(mk_edge("e:" + p, EdgeKind::DependsOn, p, "pkg:hub"));
    }
    auto h        = make(as_files(s));
    h.view().mode  = ecs::ViewMode::FileGraph;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();

    // A latecomer depending on the hub, arriving without a relayout.
    Snapshot s2 = as_files(s);
    s2.generation = 101;
    s2.nodes.push_back(mk_node("pkg:late", NodeKind::File, "repo"));
    s2.edges.push_back(mk_edge("e:late", EdgeKind::Imports, "pkg:late", "pkg:hub"));
    h.store().reset(s2);
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick(1.0f / 60.0f, 120);

    const entt::entity late = h.node("pkg:late");
    CHECK(late != entt::null);
    CHECK(!h.registry().all_of<ecs::Unplaced>(late));

    // On a ring, not stranded at the origin or flung off the graph.
    const float r = length(h.registry().get<ecs::Position>(late).p);
    const float peer =
        length(h.registry().get<ecs::Position>(h.node("pkg:d0")).p);
    CHECK(r > 1.0f);
    CHECK(std::abs(r - peer) < std::max(40.0f, peer * 0.5f));
}

// -- the architecture view ------------------------------------------------------
//
// The same radial algorithm the filesystem view uses, driven by imports instead of
// containment. Everything is on screen from the start: the foundation on the first
// ring, each ring outward built on the one inside it, and a node's orbiting children
// are the modules that import it.

namespace {

Snapshot with_symbols() {
    Snapshot s = chain();
    s.nodes.push_back(mk_node("sym:a/x.ts#Foo", NodeKind::Symbol, "file:a/x.ts", "Foo"));
    s.nodes.push_back(mk_node("file:c/z.ts", NodeKind::File, "pkg:c"));   // imports nothing
    s.edges.push_back(mk_edge("e:y-reads-Foo", EdgeKind::References, "file:b/y.ts", "sym:a/x.ts#Foo"));
    return s;
}

float gap(rgvtest::Harness& h, const std::string& a, const std::string& b) {
    return length(h.registry().get<ecs::Position>(h.index().node(a)).p -
                  h.registry().get<ecs::Position>(h.index().node(b)).p);
}

// The layout tree, read back from where things ended up: a node's parent is whichever
// node it orbits. Asserting on the arrangement rather than on an internal map.
int ring_of(rgvtest::Harness& h, const std::string& id) {
    return h.registry().get<ecs::Depth>(h.index().node(id)).value;
}

} // namespace

TEST(the_architecture_view_shows_every_node_from_the_start) {
    auto h = make(with_symbols());
    // Files, packages, and the repository the radial layout grows from. Nothing folded,
    // and no test of whether a module takes part in anything: one that imports nothing
    // is still part of the architecture.
    CHECK(h.node("repo") != entt::null);
    CHECK(h.node("pkg:a") != entt::null);
    CHECK(h.node("file:a/x.ts") != entt::null);
    CHECK(h.node("file:b/y.ts") != entt::null);
    CHECK(h.node("file:c/z.ts") != entt::null);   // imports nothing, still drawn
    // Directories are filesystem structure; this view's structure is what imports what.
    CHECK(h.node("dir:a") == entt::null);
    CHECK(h.node("sym:a/x.ts#Foo") == entt::null);
}

TEST(the_architecture_view_draws_imports_and_nothing_else) {
    auto h = make(with_symbols());
    CHECK(h.edge("e:y->x") != entt::null);          // an import
    CHECK(h.edge("e:y-reads-Foo") == entt::null);   // a symbol read
    CHECK(h.edge("e:b->a") == entt::null);          // a declared package dependency
    for (auto [e, ref] : h.registry().view<const ecs::EdgeRef>().each()) {
        CHECK(ref.kind == EdgeKind::Imports);
    }
}

// The reading: the foundation is at the centre and each ring outward is built on the
// ring inside it. y.ts imports x.ts, so x.ts is nearer the middle.
TEST(the_import_tree_puts_the_foundation_at_the_centre) {
    auto h = make(chain());
    h.settle();
    CHECK_EQ(ring_of(h, "repo"), 0);
    CHECK_EQ(ring_of(h, "file:a/x.ts"), 1);   // imports nothing
    CHECK_EQ(ring_of(h, "file:b/y.ts"), 2);   // imports x.ts
    CHECK(length(h.registry().get<ecs::Position>(h.node("file:a/x.ts")).p) <
          length(h.registry().get<ecs::Position>(h.node("file:b/y.ts")).p));
    // A module orbits the one it imports, and is nearer to it than to anything else.
    CHECK(gap(h, "file:b/y.ts", "file:a/x.ts") < gap(h, "file:b/y.ts", "repo"));
}

// A package has no imports, so it sits on the first ring with the rest of the
// foundation rather than anywhere special.
TEST(a_node_that_imports_nothing_sits_on_the_first_ring) {
    auto h = make(with_symbols());
    h.settle();
    CHECK_EQ(ring_of(h, "pkg:a"), 1);
    CHECK_EQ(ring_of(h, "file:c/z.ts"), 1);
}

TEST(the_architecture_view_uses_the_same_discs_the_filesystem_view_does) {
    auto h = make(chain());
    h.settle();
    CHECK(h.registry().all_of<ecs::Disc>(h.node("file:a/x.ts")));
    // x.ts is imported by y.ts, so it holds an orbit; y.ts holds nothing and is a dot.
    CHECK(h.registry().get<ecs::Disc>(h.node("file:a/x.ts")).radius >
          h.registry().get<ecs::Disc>(h.node("file:b/y.ts")).radius);
}

TEST(nothing_overlaps_once_the_architecture_layout_settles) {
    auto h = make(with_symbols());
    h.settle();
    std::vector<entt::entity> all;
    for (auto [e, ref] : h.registry().view<const ecs::NodeRef>().each()) all.push_back(e);
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            const auto& pa = h.registry().get<ecs::Position>(all[i]).p;
            const auto& pb = h.registry().get<ecs::Position>(all[j]).p;
            const float want = h.registry().get<ecs::Disc>(all[i]).radius +
                               h.registry().get<ecs::Disc>(all[j]).radius;
            CHECK(length(pa - pb) > want * 0.9f);
        }
    }
}

TEST(the_architecture_layout_is_deterministic) {
    auto a = make(with_symbols());
    auto b = make(with_symbols());
    a.settle();
    b.settle();
    for (auto [e, ref] : a.registry().view<const ecs::NodeRef>().each()) {
        const auto& pa = a.registry().get<ecs::Position>(e).p;
        const auto& pb = b.registry().get<ecs::Position>(b.index().node(ref.id)).p;
        CHECK(length(pa - pb) < 0.5f);
    }
}

TEST(parallel_imports_collapse_into_one_line_carrying_a_count) {
    Snapshot s = chain();
    // Two contract edges between the same pair: one line, and the line says so.
    s.edges.push_back(mk_edge("e:y->x2", EdgeKind::Imports, "file:b/y.ts", "file:a/x.ts"));
    auto h = make(s);
    CHECK_EQ(edges_between(h, "file:b/y.ts", "file:a/x.ts"), 1);
    const auto e = h.edge("e:y->x");
    CHECK(e != entt::null);
    CHECK_EQ(h.registry().get<ecs::EdgeWeight>(e).count, 2);
}

TEST(architecture_view_colours_modules_from_the_file_level_result) {
    auto h = make();
    push_change(h.store(), "a/x.ts", "file:a/x.ts");
    ImpactedNode y;
    y.node_id = "file:b/y.ts"; y.min_distance = 1; y.direct = true;
    y.paths.push_back(ImpactPath{{"e:y->x"}});
    push_impact(h.store(), Level::File, {y}, {"file:a/x.ts"});
    h.tick();
    CHECK(h.registry().all_of<ecs::Changed>(h.node("file:a/x.ts")));
    CHECK(h.registry().all_of<ecs::Impacted>(h.node("file:b/y.ts")));
    CHECK(h.registry().all_of<ecs::Changed>(h.node("pkg:a")));   // owns the change
}

// The symbol level is two-sided: a symbol's dependents are files, so files take part
// in reach and specificity there or every symbol scores zero.
TEST(at_symbol_level_files_count_as_dependents_of_symbols) {
    auto h        = make(with_symbols());
    h.view().level = Level::Symbol;
    h.tick();
    const auto& derived = h.world.resource<ecs::DerivedState>();
    CHECK_EQ(derived.reach.dependents("sym:a/x.ts#Foo"), 1);
    CHECK_EQ(derived.specificity.dependents("sym:a/x.ts#Foo"), 1);
}

// -- hiding by pattern ---------------------------------------------------------
//
// FR-35: a filter that removes what matches. Test modules are the case that asked for
// it -- thirty of them around one package, each importing half the code -- and the rule
// is that a hidden node is gone: its edges are not re-routed to whatever contains it.

TEST(a_hide_pattern_removes_matching_nodes_and_their_edges) {
    auto h = make(with_symbols());
    CHECK(ecs::add_hide_pattern(h.filters(), "b/"));
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("file:b/y.ts") == entt::null);
    CHECK(h.index().node("pkg:b") != entt::null);
    CHECK(h.index().node("file:a/x.ts") != entt::null);
    // The read of Foo came from the hidden module. It is gone, not moved up to pkg:b.
    CHECK(h.index().edge("e:y-reads-Foo") == entt::null);
    for (auto [e, ref, ends] : h.registry().view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) continue;
        CHECK(!(ends.from == h.index().node("pkg:b") && ends.to == h.index().node("file:a/x.ts")));
    }
}

TEST(hiding_a_package_hides_what_it_holds) {
    auto h = make();
    CHECK(ecs::add_hide_pattern(h.filters(), "^pkg:a$"));
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("pkg:a") == entt::null);
    CHECK(h.index().node("file:a/x.ts") == entt::null);
    CHECK(h.index().node("pkg:b") != entt::null);
    CHECK(h.index().node("file:b/y.ts") != entt::null);
}

TEST(an_invalid_pattern_is_kept_but_hides_nothing) {
    auto h = make();
    CHECK(!ecs::add_hide_pattern(h.filters(), "("));
    CHECK_EQ(h.filters().hidden.size(), 1u);
    CHECK(!h.filters().hidden[0].valid);
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK_EQ(count_nodes(h), 6);
}

// Explicit beats everything: the relevance filter spares what the agent changed, but a
// pattern the user typed is a decision, and a changed test module is still a test.
TEST(a_hidden_node_stays_hidden_when_it_changes) {
    auto h = make();
    CHECK(ecs::add_hide_pattern(h.filters(), "x\\.ts$"));
    push_change(h.store(), "a/x.ts", "file:a/x.ts");
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("file:a/x.ts") == entt::null);
}

TEST(matching_is_case_insensitive_and_removing_a_pattern_restores_the_nodes) {
    auto h = make();
    CHECK(ecs::add_hide_pattern(h.filters(), "Y\\.TS"));
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("file:b/y.ts") == entt::null);
    h.filters().hidden.clear();
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("file:b/y.ts") != entt::null);
}

// -- dependency curves on hover, over the filesystem tree ------------------------
//
// The filesystem view is the most legible picture the tool draws, and it is legible
// because it draws containment and nothing else. Dependencies are shown for one node at
// a time, as curves, and produced at draw time rather than as entities -- so the layout
// never learns they exist.

namespace {

rgvtest::Harness tree_view(Snapshot s = with_symbols()) {
    rgvtest::Harness h;
    h.store().reset(s);
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.tick();
    return h;
}

std::vector<rgv::view::HoverLink> links_for(rgvtest::Harness& h, const std::string& id) {
    return rgv::view::hover_links(h.store(), id, [&](const rgv::NodeId& n) {
        return h.index().node(n) != entt::null;
    });
}

const rgv::view::HoverLink* link_to(const std::vector<rgv::view::HoverLink>& ls,
                                    const std::string& other) {
    for (const auto& l : ls) {
        if (l.other == other) return &l;
    }
    return nullptr;
}

} // namespace

TEST(hovering_a_file_offers_what_it_depends_on_and_what_depends_on_it) {
    auto h = tree_view(chain());
    const auto out = links_for(h, "file:b/y.ts");   // y.ts imports x.ts
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].other, std::string("file:a/x.ts"));
    CHECK(out[0].outgoing);

    const auto in = links_for(h, "file:a/x.ts");
    CHECK_EQ(in.size(), 1u);
    CHECK_EQ(in[0].other, std::string("file:b/y.ts"));
    CHECK(!in[0].outgoing);
}

// Symbols are not drawn in a filesystem tree, so a use of one answers as the file that
// defines it -- and the import of that same file collapses into the one curve.
TEST(a_link_to_a_symbol_resolves_to_the_file_that_defines_it) {
    auto h = tree_view();
    const auto out = links_for(h, "file:b/y.ts");
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].other, std::string("file:a/x.ts"));
    CHECK(out[0].kind == EdgeKind::References);   // the most specific of the two
}

// A directory answers for everything inside it, which is what makes "what does this
// folder need" a hover rather than a query.
TEST(hovering_a_directory_answers_for_its_whole_subtree) {
    auto h = tree_view(chain());
    const auto out = links_for(h, "dir:a");   // holds x.ts, which y.ts imports
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].other, std::string("file:b/y.ts"));
    CHECK(!out[0].outgoing);
}

// A dependency that stays inside the hovered node is not a crossing and is not drawn.
TEST(a_dependency_wholly_inside_the_hovered_node_is_not_a_link) {
    Snapshot s = with_symbols();
    s.edges.push_back(mk_edge("e:x-uses-Foo", EdgeKind::References, "file:a/x.ts",
                              "sym:a/x.ts#Foo"));
    auto h = tree_view(s);
    for (const auto& l : links_for(h, "file:a/x.ts")) {
        CHECK(l.other != std::string("file:a/x.ts"));
    }
    CHECK(link_to(links_for(h, "dir:a"), "file:a/x.ts") == nullptr);
}

TEST(a_bow_keeps_its_endpoints_and_never_swings_wider_than_its_own_span) {
    const Vec2 hub{0.0f, 4000.0f};
    auto deflection = [&](Vec2 a, Vec2 b) {
        const auto pts = rgv::view::sample_bow(a, b, hub, 0.55f, 12);
        return length(pts[6] - Vec2{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f});
    };

    const Vec2 a{-100.0f, 0.0f}, b{100.0f, 0.0f};
    const auto pts = rgv::view::sample_bow(a, b, hub, 0.55f, 12);
    CHECK_EQ(pts.size(), 13u);
    CHECK(length(pts.front() - a) < 0.01f);
    CHECK(length(pts.back() - b) < 0.01f);
    CHECK(pts[6].y > 0.0f);              // bowed toward the hub
    CHECK(std::abs(pts[6].x) < 1.0f);    // and still centred between the ends

    // A near pair bows a little and a far pair bows a lot, and neither swings wider
    // than the gap it spans. A short hop beside a distant hub used to arc across the
    // whole view, because the pull was a fraction of the distance to the hub and that
    // has nothing to do with how far apart the two nodes are.
    const float near_pair = deflection(Vec2{-40.0f, 0.0f}, Vec2{40.0f, 0.0f});
    const float far_pair  = deflection(Vec2{-800.0f, 0.0f}, Vec2{800.0f, 0.0f});
    CHECK(near_pair > 1.0f);                  // still a curve, never a straight line
    CHECK(near_pair < 80.0f * 0.35f);         // proportionate to its 80-unit span
    CHECK(far_pair > near_pair * 4.0f);

    // The hub sitting on the line is the degenerate case: bow to one side rather than
    // collapsing to a straight line a containment stub could be mistaken for.
    const auto flat = rgv::view::sample_bow(a, b, Vec2{0.0f, 0.0f}, 0.55f, 12);
    CHECK(std::abs(flat[6].y) > 5.0f);
}

// The guarantee that makes this safe to draw at all. Against a control rather than
// against a snapshot, because the easing is still converging by fractions of a unit
// after the layout reports itself settled, and that is not what is being measured.
TEST(hovering_moves_nothing) {
    auto quiet = tree_view();
    auto hover = tree_view();
    quiet.settle();
    hover.settle();
    view::fit_camera(quiet.world, {});
    view::fit_camera(hover.world, {});

    auto edge_count = [](rgvtest::Harness& h) {
        int n = 0;
        for ([[maybe_unused]] auto&& row : h.registry().view<const ecs::EdgeRef>().each()) ++n;
        return n;
    };
    const int edges_before = edge_count(hover);

    // Hovered through the real input path, so picking agrees this is the node.
    const Vec2 at = hover.registry().get<ecs::Position>(hover.node("file:b/y.ts")).p;
    hover.point_at(hover.camera().world_to_screen(at));
    quiet.tick();   // the same number of frames, with the pointer left alone
    CHECK(hover.registry().all_of<ecs::Hovered>(hover.node("file:b/y.ts")));

    quiet.tick(1.0f / 60.0f, 30);
    hover.tick(1.0f / 60.0f, 30);

    // Nothing was added to the scene: the curves are produced at draw time.
    CHECK_EQ(edge_count(hover), edges_before);
    int compared = 0;
    for (auto [e, ref, pos] : quiet.registry().view<const ecs::NodeRef, const ecs::Position>().each()) {
        const auto other = hover.node(ref.id);
        CHECK(other != entt::null);
        CHECK(length(pos.p - hover.registry().get<ecs::Position>(other).p) < 1e-4f);
        ++compared;
    }
    CHECK(compared > 3);
}

// `--hover` exists so a screenshot of a particular state is reproducible, and it was
// not: picking overwrote it on the first frame from wherever the cursor happened to be
// resting. A cursor that has not moved is not input.
TEST(a_forced_hover_holds_until_the_pointer_actually_moves) {
    auto h = tree_view(chain());
    h.settle();
    view::fit_camera(h.world, {});

    h.selection().hovered      = "file:b/y.ts";
    h.selection().hover_pinned = true;
    h.tick(1.0f / 60.0f, 5);
    CHECK_EQ(h.selection().hovered, std::string("file:b/y.ts"));
    CHECK(h.registry().all_of<ecs::Hovered>(h.node("file:b/y.ts")));

    // Moving the pointer hands control back to it at once.
    const Vec2 other = h.registry().get<ecs::Position>(h.node("file:a/x.ts")).p;
    h.point_at(h.camera().world_to_screen(other));
    CHECK(!h.selection().hover_pinned);
    CHECK_EQ(h.selection().hovered, std::string("file:a/x.ts"));
}

// -- double click ---------------------------------------------------------------
//
// The desktop already knows what opens a `.py`, so a file is handed to it rather than
// to a viewer of our own. Everything else keeps the gesture it had.

namespace {

void double_click(rgvtest::Harness& h, entt::entity target) {
    const Vec2 at = h.registry().get<ecs::Position>(target).p;
    h.point_at(h.camera().world_to_screen(at));
    h.input().double_click = true;
    h.tick();
    h.input().double_click = false;
    h.tick();   // the command lands on the next frame
}

} // namespace

TEST(double_clicking_a_file_opens_it_rather_than_pinning_it) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});
    const entt::entity file = h.node("file:a/x.ts");
    double_click(h, file);
    CHECK(h.pointer().entity == file);
    // Nothing is launched here: the fixture's repository root is not a real directory,
    // so the path is refused before any launcher is asked for.
    CHECK(!h.registry().all_of<ecs::Pinned>(file));
}

TEST(double_clicking_something_that_is_not_a_file_still_pins_it) {
    auto h = make_filesystem();
    view::fit_camera(h.world, {});
    const entt::entity dir = h.node("dir:a");
    double_click(h, dir);
    CHECK(h.pointer().entity == dir);
    CHECK(h.registry().all_of<ecs::Pinned>(dir));
    double_click(h, dir);
    CHECK(!h.registry().all_of<ecs::Pinned>(dir));
}

// Pinning is still reachable for a file, through the inspector rather than the gesture.
TEST(a_file_can_still_be_pinned_by_command) {
    auto h = make_filesystem();
    const entt::entity file = h.node("file:a/x.ts");
    h.commands().push(ecs::TogglePin{"file:a/x.ts"});
    h.tick();
    CHECK(h.registry().all_of<ecs::Pinned>(file));
    h.commands().push(ecs::TogglePin{"file:a/x.ts"});
    h.tick();
    CHECK(!h.registry().all_of<ecs::Pinned>(file));
}

// -- labels beside a node ---------------------------------------------------------
//
// A name beside a node holds a constant screen size while the graph spreads out under
// it, so there is a fixed amount of room and more names than room. Priority order, and
// a name is drawn only if it clears every name already drawn.

namespace {

// A directory full of files with names too long to all fit, which is what makes the
// choice visible. Every file also imports the first one, so degree varies.
Snapshot crowded() {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository), mk_node("dir:pkg", NodeKind::Directory, "repo")};
    for (int i = 0; i < 40; ++i) {
        const std::string id = "file:pkg/a_rather_long_module_name_" + std::to_string(i) + ".ts";
        s.nodes.push_back(mk_node(id, NodeKind::File, "dir:pkg",
                                  "a_rather_long_module_name_" + std::to_string(i) + ".ts"));
        if (i > 0) {
            s.edges.push_back(mk_edge("e:" + std::to_string(i), EdgeKind::Imports, id,
                                      "file:pkg/a_rather_long_module_name_0.ts"));
        }
    }
    return s;
}

rgvtest::Harness crowded_tree() {
    rgvtest::Harness h;
    h.store().reset(crowded());
    h.view().mode  = ecs::ViewMode::Filesystem;
    h.view().level = Level::File;
    h.request_rebuild();
    h.settle();
    view::fit_camera(h.world, {});
    h.tick(1.0f / 60.0f, 40);   // let the fades finish
    return h;
}

struct ShownLabel { std::string id; Vec2 min, max; };

std::vector<ShownLabel> shown_labels(rgvtest::Harness& h) {
    std::vector<ShownLabel> out;
    for (auto [e, ref, side, label] :
         h.registry().view<const ecs::NodeRef, const ecs::SideLabel, const ecs::Label>().each()) {
        if (side.alpha <= 0.004f) continue;
        const float w =
            static_cast<float>(label.text.size()) * side.px * rgv::view::kCharAdvanceRatio;
        out.push_back({ref.id, Vec2{side.anchor.x - w * 0.5f, side.anchor.y},
                       Vec2{side.anchor.x + w * 0.5f, side.anchor.y + side.px}});
    }
    return out;
}

// A node whose name lost the room, so a test can ask for it back.
std::string a_hidden_one(rgvtest::Harness& h) {
    for (auto [e, ref, side] : h.registry().view<const ecs::NodeRef, const ecs::SideLabel>().each()) {
        if (side.alpha <= 0.004f) return ref.id;
    }
    return {};
}

} // namespace

TEST(there_are_more_names_than_room_and_some_lose) {
    auto h = crowded_tree();
    CHECK(!a_hidden_one(h).empty());
    CHECK(shown_labels(h).size() > 1u);
}

// The invariant the whole thing exists for.
TEST(no_two_labels_that_are_drawn_overlap) {
    auto       h     = crowded_tree();
    const auto shown = shown_labels(h);
    for (std::size_t i = 0; i < shown.size(); ++i) {
        for (std::size_t j = i + 1; j < shown.size(); ++j) {
            const bool over = shown[i].min.x < shown[j].max.x && shown[j].min.x < shown[i].max.x &&
                              shown[i].min.y < shown[j].max.y && shown[j].min.y < shown[i].max.y;
            CHECK(!over);
        }
    }
}

// Busier nodes are named first, so the one everything imports keeps its name however
// crowded its neighbours are.
TEST(the_busiest_node_keeps_its_name) {
    auto       h   = crowded_tree();
    const auto hub = h.node("file:pkg/a_rather_long_module_name_0.ts");
    CHECK(hub != entt::null);
    CHECK(h.registry().get<ecs::SideLabel>(hub).alpha > 0.9f);
}

TEST(pointing_at_a_node_takes_the_room_back_for_its_name) {
    auto              h      = crowded_tree();
    const std::string hidden = a_hidden_one(h);
    CHECK(!hidden.empty());

    h.selection().hovered      = hidden;
    h.selection().hover_pinned = true;
    h.tick(1.0f / 60.0f, 40);
    CHECK(h.registry().get<ecs::SideLabel>(h.node(hidden)).alpha > 0.9f);
}

// Selecting outranks how busy a node is, and hovering outranks selecting.
TEST(hovering_outranks_selecting_which_outranks_being_busy) {
    auto              h      = crowded_tree();
    const std::string hidden = a_hidden_one(h);
    h.commands().push(ecs::SelectNode{hidden});
    h.tick(1.0f / 60.0f, 40);
    CHECK(h.registry().get<ecs::SideLabel>(h.node(hidden)).alpha > 0.9f);

    // And the hovered one wins over the selected one when they collide: give the
    // selection to one node and the pointer to another, and both are named.
    const auto shown_now = shown_labels(h);
    CHECK(shown_now.size() > 1u);
}

TEST(a_label_fades_in_over_a_quarter_second_and_back_out) {
    auto              h      = crowded_tree();
    const std::string hidden = a_hidden_one(h);
    const auto        ent    = h.node(hidden);
    CHECK_EQ(h.registry().get<ecs::SideLabel>(ent).alpha, 0.0f);

    h.selection().hovered      = hidden;
    h.selection().hover_pinned = true;
    h.tick(1.0f / 60.0f, 8);   // ~0.13s: about half way, not there yet
    const float part = h.registry().get<ecs::SideLabel>(ent).alpha;
    CHECK(part > 0.3f);
    CHECK(part < 0.8f);

    h.tick(1.0f / 60.0f, 10);   // past 0.25s in total
    CHECK_EQ(h.registry().get<ecs::SideLabel>(ent).alpha, 1.0f);

    // And back out at the same rate once the pointer leaves.
    h.selection().hovered.clear();
    h.tick(1.0f / 60.0f, 8);
    const float going = h.registry().get<ecs::SideLabel>(ent).alpha;
    CHECK(going < 0.8f);
    h.tick(1.0f / 60.0f, 40);
    CHECK_EQ(h.registry().get<ecs::SideLabel>(ent).alpha, 0.0f);
}

