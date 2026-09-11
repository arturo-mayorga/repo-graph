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

// Opens the named packages, so a test can look at the modules inside them. The
// architecture view starts at package level, so most module-level assertions need it.
void expand(rgvtest::Harness& h, std::initializer_list<const char*> ids) {
    for (const auto* id : ids) h.commands().push(ecs::ToggleExpand{id});
    h.tick();
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
    CHECK_EQ(count_nodes(h), 3);   // three packages; their modules are folded into them

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
    CHECK_EQ(h.stats().changed, 1);   // the package that owns it; the module is folded in

    // Opened, the module that actually changed is marked too.
    expand(h, {"pkg:a"});
    CHECK(h.registry().all_of<ecs::Changed>(h.node("file:a/x.ts")));
    CHECK_EQ(h.stats().changed, 2);
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
    // b -> a is not drawn: the module edge y -> x between their contents explains it,
    // so that is what lights up for the hop.
    CHECK(h.edge("e:b->a") == entt::null);
    CHECK(on_path("e:y->x", true));
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
    CHECK(!view::node_detail(h.camera().zoom, h.view().graph_text_scale).labels);

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
// A package is a node linked to the modules inside it, and the layout is force-directed
// over those links: containment attracts, everything repels. That is what makes "the
// code around each system" visible in the view that opens by default -- the modules
// gather around their package -- without boxes that get in the way of panning.

namespace {

Snapshot with_symbols() {
    Snapshot s = chain();
    s.nodes.push_back(mk_node("sym:a/x.ts#Foo", NodeKind::Symbol, "file:a/x.ts", "Foo"));
    s.nodes.push_back(mk_node("file:c/z.ts", NodeKind::File, "pkg:c"));   // no dependencies: not architecture
    s.edges.push_back(mk_edge("e:y-reads-Foo", EdgeKind::References, "file:b/y.ts", "sym:a/x.ts#Foo"));
    return s;
}

float gap(rgvtest::Harness& h, const std::string& a, const std::string& b) {
    return length(h.registry().get<ecs::Position>(h.index().node(a)).p -
                  h.registry().get<ecs::Position>(h.index().node(b)).p);
}

} // namespace

TEST(architecture_view_links_modules_to_their_packages_and_keeps_them_close) {
    auto h = make();
    expand(h, {"pkg:a", "pkg:b", "pkg:c"});
    h.settle();
    CHECK(h.index().node("file:a/x.ts") != entt::null);
    CHECK(h.index().node("file:b/y.ts") != entt::null);
    // Containment is on screen as an edge, which is what the layout pulls along.
    CHECK(h.index().edge(std::string("tree:file:a/x.ts")) != entt::null);
    // Each module ends up nearer its own package than any other.
    CHECK(gap(h, "file:a/x.ts", "pkg:a") < gap(h, "file:a/x.ts", "pkg:b"));
    CHECK(gap(h, "file:a/x.ts", "pkg:a") < gap(h, "file:a/x.ts", "pkg:c"));
    CHECK(gap(h, "file:b/y.ts", "pkg:b") < gap(h, "file:b/y.ts", "pkg:a"));
}

TEST(nothing_overlaps_once_the_architecture_layout_settles) {
    auto h = make(with_symbols());
    expand(h, {"pkg:a", "pkg:b", "pkg:c"});
    h.settle();
    std::vector<entt::entity> all;
    for (auto [e, ref] : h.registry().view<const ecs::NodeRef>().each()) all.push_back(e);
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            const auto& pa = h.registry().get<ecs::Position>(all[i]).p;
            const auto& pb = h.registry().get<ecs::Position>(all[j]).p;
            const auto& xa = h.registry().get<ecs::Extent>(all[i]).half;
            const auto& xb = h.registry().get<ecs::Extent>(all[j]).half;
            const bool apart = std::abs(pa.x - pb.x) >= (xa.x + xb.x) * 0.9f ||
                               std::abs(pa.y - pb.y) >= (xa.y + xb.y) * 0.9f;
            CHECK(apart);
        }
    }
}

// Same graph, same picture. A layout that depends on where the scene happened to leave
// things is a layout nobody can compare across two runs.
TEST(the_architecture_layout_is_deterministic) {
    auto a = make(with_symbols());
    auto b = make(with_symbols());
    expand(a, {"pkg:a", "pkg:b"});
    expand(b, {"pkg:a", "pkg:b"});
    a.settle();
    b.settle();
    for (auto [e, ref] : a.registry().view<const ecs::NodeRef>().each()) {
        const auto& pa = a.registry().get<ecs::Position>(e).p;
        const auto& pb = b.registry().get<ecs::Position>(b.index().node(ref.id)).p;
        CHECK(length(pa - pb) < 0.5f);
    }
}

TEST(a_file_with_no_dependencies_is_not_architecture) {
    auto h = make(with_symbols());
    expand(h, {"pkg:a", "pkg:c"});
    CHECK(h.index().node("file:c/z.ts") == entt::null);
    CHECK(h.index().node("file:a/x.ts") != entt::null);
}

// The edge between two modules explains the edge between their packages, so the
// package edge is not drawn on top of it. A package with no modules on screen keeps its
// own edge: there is nothing else to say it.
TEST(module_edges_replace_the_package_edge_they_explain) {
    auto h = make();
    CHECK(h.index().edge("e:y->x") != entt::null);   // b/y.ts imports a/x.ts
    CHECK(h.index().edge("e:b->a") == entt::null);   // explained by it
    CHECK(h.index().edge("e:c->b") != entt::null);   // c holds nothing on screen
}

// Several store edges land between the same two modules -- the import of a file and
// every read of a symbol inside it. The most specific one is drawn, between the files.
TEST(a_symbol_use_is_drawn_between_the_files_and_wins_over_the_import) {
    auto h = make(with_symbols());
    expand(h, {"pkg:a", "pkg:b"});
    const auto e = h.index().edge("e:y-reads-Foo");
    CHECK(e != entt::null);
    CHECK(h.index().edge("e:y->x") == entt::null);
    const auto& ends = h.registry().get<ecs::Endpoints>(e);
    CHECK(ends.from == h.index().node("file:b/y.ts"));
    CHECK(ends.to == h.index().node("file:a/x.ts"));   // the symbol's file stands for it
}

// A changed module is red beside its package, and the module that imports it is lit
// from the file-level result even while the view reads package-level impact.
TEST(architecture_view_colours_modules_from_the_file_level_result) {
    auto h = make();
    expand(h, {"pkg:a", "pkg:b"});
    push_change(h.store(), "a/x.ts", "file:a/x.ts");
    ImpactedNode y;
    y.node_id = "file:b/y.ts"; y.min_distance = 1; y.direct = true;
    y.paths.push_back(ImpactPath{{"e:y->x"}});
    push_impact(h.store(), Level::File, {y}, {"file:a/x.ts"});
    h.tick();
    CHECK(h.registry().all_of<ecs::Changed>(h.index().node("file:a/x.ts")));
    CHECK(h.registry().all_of<ecs::Impacted>(h.index().node("file:b/y.ts")));
    CHECK(h.registry().all_of<ecs::Changed>(h.index().node("pkg:a")));   // owns the change
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
    expand(h, {"pkg:a", "pkg:b"});
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
    expand(h, {"pkg:a", "pkg:b"});
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
    CHECK_EQ(count_nodes(h), 3);
}

// Explicit beats everything: the relevance filter spares what the agent changed, but a
// pattern the user typed is a decision, and a changed test module is still a test.
TEST(a_hidden_node_stays_hidden_when_it_changes) {
    auto h = make();
    expand(h, {"pkg:a"});
    CHECK(ecs::add_hide_pattern(h.filters(), "x\\.ts$"));
    push_change(h.store(), "a/x.ts", "file:a/x.ts");
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("file:a/x.ts") == entt::null);
}

TEST(matching_is_case_insensitive_and_removing_a_pattern_restores_the_nodes) {
    auto h = make();
    expand(h, {"pkg:b"});
    CHECK(ecs::add_hide_pattern(h.filters(), "Y\\.TS"));
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("file:b/y.ts") == entt::null);
    h.filters().hidden.clear();
    h.world.resource<ecs::SceneRequests>().revisit = true;
    h.tick();
    CHECK(h.index().node("file:b/y.ts") != entt::null);
}

// -- package level, and expanding one --------------------------------------------
//
// A system design diagram has ten boxes, not a hundred. The architecture view opens at
// package level for the same reason: 460 file-level edges over 104 nodes cannot be
// drawn without crossings by ANY layout -- that is Euler's bound, not a layout defect --
// while the same graph aggregated to packages is 27 edges and reads like the mermaid
// charts in a repository's own docs.

namespace {

Snapshot relations() {
    Snapshot s;
    s.generation                  = 100;
    s.session.baseline_generation = 100;
    s.nodes = {mk_node("repo", NodeKind::Repository),
               mk_node("pkg:sys", NodeKind::Package, "repo"),
               mk_node("pkg:comp", NodeKind::Package, "repo"),
               mk_node("file:sys/m.py", NodeKind::File, "pkg:sys"),
               mk_node("file:comp/c.py", NodeKind::File, "pkg:comp"),
               mk_node("sym:comp/c.py#C", NodeKind::Symbol, "file:comp/c.py", "C")};
    // One of each relation, all landing on the same pair of packages.
    s.edges = {mk_edge("e:imp", EdgeKind::Imports, "file:sys/m.py", "file:comp/c.py"),
               mk_edge("e:write", EdgeKind::Calls, "file:sys/m.py", "sym:comp/c.py#C"),
               mk_edge("e:read", EdgeKind::References, "file:sys/m.py", "sym:comp/c.py#C")};
    return s;
}

} // namespace

TEST(the_architecture_view_opens_at_package_level) {
    auto h = make();
    CHECK(h.node("pkg:a") != entt::null);
    CHECK(h.node("pkg:b") != entt::null);
    CHECK(h.node("file:a/x.ts") == entt::null);
    CHECK(h.node("file:b/y.ts") == entt::null);
    CHECK_EQ(count_nodes(h), 3);

    // The import between two modules is carried by the packages that hold them.
    const auto e = h.edge("e:y->x");
    CHECK(e != entt::null);
    const auto& ends = h.registry().get<ecs::Endpoints>(e);
    CHECK(ends.from == h.node("pkg:b"));
    CHECK(ends.to == h.node("pkg:a"));
}

TEST(expanding_a_package_reveals_its_modules_and_moves_the_edge_onto_them) {
    auto h = make();
    expand(h, {"pkg:a"});
    CHECK(h.node("file:a/x.ts") != entt::null);
    CHECK(h.node("file:b/y.ts") == entt::null);   // b is still collapsed

    // One end moved down to the module; the other is still the package.
    const auto& ends = h.registry().get<ecs::Endpoints>(h.edge("e:y->x"));
    CHECK(ends.from == h.node("pkg:b"));
    CHECK(ends.to == h.node("file:a/x.ts"));

    expand(h, {"pkg:a"});   // collapses again
    CHECK(h.node("file:a/x.ts") == entt::null);
}

// A module that takes part in nothing is not architecture even once its package opens.
TEST(expanding_a_package_still_leaves_out_what_takes_part_in_nothing) {
    auto h = make(with_symbols());
    expand(h, {"pkg:a", "pkg:c"});
    CHECK(h.node("file:a/x.ts") != entt::null);
    CHECK(h.node("file:c/z.ts") == entt::null);
}

TEST(parallel_edges_collapse_into_one_line_carrying_a_count) {
    auto h = make(relations());
    CHECK_EQ(edges_between(h, "pkg:sys", "pkg:comp"), 1);

    // Three store edges behind one line, and the line says so.
    const auto e = h.edge("e:write");   // the most specific of the three
    CHECK(e != entt::null);
    CHECK_EQ(h.registry().get<ecs::EdgeWeight>(e).count, 3);

    // Expanded, the same three still collapse onto one pair of modules.
    expand(h, {"pkg:sys", "pkg:comp"});
    CHECK_EQ(edges_between(h, "file:sys/m.py", "file:comp/c.py"), 1);
}

TEST(the_relation_filter_draws_only_the_chosen_kind) {
    auto h = make(relations());
    struct Case { ecs::Relation r; const char* id; int weight; };
    for (const auto& c : {Case{ecs::Relation::Imports, "e:imp", 1},
                          Case{ecs::Relation::Reads, "e:read", 1},
                          Case{ecs::Relation::Writes, "e:write", 1},
                          Case{ecs::Relation::All, "e:write", 3}}) {
        h.filters().relation = c.r;
        h.world.resource<ecs::SceneRequests>().revisit = true;
        h.tick();
        CHECK_EQ(edges_between(h, "pkg:sys", "pkg:comp"), 1);
        CHECK(h.edge(c.id) != entt::null);
        CHECK_EQ(h.registry().get<ecs::EdgeWeight>(h.edge(c.id)).count, c.weight);
    }
}

// Reading direction. A package depending on nothing sits at the bottom and its
// dependents stack above it, so the eye can follow impact upward without a legend.
TEST(the_architecture_layout_ranks_dependencies_into_a_reading_direction) {
    auto h = make();
    h.settle();
    auto y = [&](const char* id) { return h.registry().get<ecs::Position>(h.node(id)).p.y; };
    // Screen y grows downward, so "above" is a smaller y. c -> b -> a.
    CHECK(y("pkg:a") > y("pkg:b"));
    CHECK(y("pkg:b") > y("pkg:c"));
}

