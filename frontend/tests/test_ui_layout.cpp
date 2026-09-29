// Panel layout invariants, driven through real ImGui frames with no window and no GPU.
//
// These exist because of a specific, easy-to-reintroduce bug: a control that edits the
// UI text scale, while being scaled by it, resizes and moves under the cursor as you
// drag it, making a particular value impossible to hit. The fix is that the text-size
// window's own geometry is independent of the scale it edits, and that is exactly what
// is asserted here.
//
// The second half of the file holds a different invariant with the same shape: a panel
// reads resources and pushes commands, and never writes anything. Most of that is now
// enforced by the compiler -- `ui::Ui` hands panels const references -- but a const_cast
// or a second, non-const route to a resource would slip past it, so the behaviour is
// pinned here too: press a control, and the state must not have moved.
#include "TestMain.h"

#include "Harness.h"

#include "rgv/ecs/Commands.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/ui/Panels.h"
#include "rgv/ui/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <variant>

using namespace rgv;

namespace {

// Headless ImGui. NewFrame only needs a display size and a built font atlas; the
// atlas is the embedded default font, so nothing has to be uploaded anywhere.
struct HeadlessImGui {
    HeadlessImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.DisplaySize = ImVec2(1920.0f, 1200.0f);
        io.DeltaTime   = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        ui::apply_imgui_style();

        unsigned char* pixels = nullptr;
        int            w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);   // builds the atlas
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(static_cast<std::intptr_t>(1)));
    }
    ~HeadlessImGui() { ImGui::DestroyContext(); }
};

// A real world with a fixture loaded, plus the panels' own resources. The panels are
// driven exactly as the application drives them.
struct UiHarness {
    rgvtest::Harness h;
    std::unique_ptr<fixture::FixtureSource> source;

    explicit UiHarness(const std::string& dir) {
        source = std::make_unique<fixture::FixtureSource>(fixture::FixtureSet::load(dir), 0);
        auto& handle    = h.world.resource<ecs::SourceHandle>();
        handle.source   = source.get();
        handle.fixtures = source.get();

        h.world.resource<ecs::FixtureLibrary>().names = {"monorepo-ts"};
        h.store().reset(source->baseline());
        h.world.resource<ecs::Viewport>().free_origin = Vec2{330.0f, 64.0f};
        h.request_rebuild();
        h.settle();
    }

    ecs::World& world() { return h.world; }
};

struct WindowGeometry {
    bool   found       = false;
    ImVec2 size{};
    ImVec2 content{};
    float  effective_font_scale = 0.0f;
};

// Runs several frames so auto-sizing and first-appearance positioning settle, then
// reports the window's geometry.
template <class Draw>
WindowGeometry measure(float ui_scale, const char* window_name, Draw&& draw) {
    ImGuiIO& io = ImGui::GetIO();
    for (int frame = 0; frame < 4; ++frame) {
        io.FontGlobalScale = ui_scale;
        ImGui::NewFrame();
        draw();
        ImGui::Render();
    }

    WindowGeometry g;
    if (ImGuiWindow* w = ImGui::FindWindowByName(window_name)) {
        g.found   = true;
        g.size    = w->Size;
        g.content = w->ContentSize;
        // The product is the invariant: whatever the global scale, this window draws
        // at the base font size.
        g.effective_font_scale = w->FontWindowScale * io.FontGlobalScale;
    }
    return g;
}

bool close_enough(const ImVec2& a, const ImVec2& b, float eps = 0.5f) {
    return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps;
}

const std::string kFixture = std::string(RGV_FIXTURE_DIR) + "/monorepo-ts";

} // namespace

// The bug, stated as a test: at two very different UI scales the text-size window must
// come out exactly the same size, with exactly the same content extent. If it does
// not, its controls move while you drag them.
TEST(the_text_size_window_geometry_does_not_depend_on_the_scale_it_edits) {
    HeadlessImGui imgui;
    UiHarness u(kFixture);
    u.world().resource<ecs::ViewSettings>().show_text_settings = true;

    u.world().resource<ecs::ViewSettings>().ui_text_scale = 1.0f;
    const auto small =
        measure(1.0f, "##textsettings", [&] { ui::draw_text_settings(u.world()); });

    u.world().resource<ecs::ViewSettings>().ui_text_scale = 2.2f;
    const auto large =
        measure(2.2f, "##textsettings", [&] { ui::draw_text_settings(u.world()); });

    CHECK(small.found);
    CHECK(large.found);
    CHECK(close_enough(small.size, large.size));
    CHECK(close_enough(small.content, large.content));
}

TEST(the_text_size_window_renders_at_the_base_font_size_whatever_the_ui_scale) {
    HeadlessImGui imgui;
    UiHarness u(kFixture);
    u.world().resource<ecs::ViewSettings>().show_text_settings = true;

    for (float scale : {0.7f, 1.0f, 1.6f, 2.5f}) {
        u.world().resource<ecs::ViewSettings>().ui_text_scale = scale;
        const auto g =
            measure(scale, "##textsettings", [&] { ui::draw_text_settings(u.world()); });
        CHECK(g.found);
        CHECK(std::abs(g.effective_font_scale - 1.0f) < 1e-3f);
    }
}

// Negative control. Without the correction the geometry really does move with the
// scale -- so the tests above are capable of failing, rather than passing vacuously.
TEST(an_uncorrected_window_would_change_size_with_the_ui_scale) {
    HeadlessImGui imgui;

    auto draw_naive = [] {
        ImGui::SetNextWindowSizeConstraints(ImVec2(372.0f, 0.0f), ImVec2(372.0f, FLT_MAX));
        ImGui::Begin("##naive", nullptr,
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted("UI text");
        ImGui::TextUnformatted("Panels, inspector, event log.");
        float v = 1.0f;
        ImGui::SliderFloat("##v", &v, 0.7f, 2.5f, "%.2fx");
        ImGui::End();
    };

    const auto small = measure(1.0f, "##naive", draw_naive);
    const auto large = measure(2.2f, "##naive", draw_naive);

    CHECK(small.found);
    CHECK(large.found);
    CHECK(!close_enough(small.size, large.size));   // this is the bug being guarded against
}

// The panels either side of the graph are meant to scale, and the graph area is
// whatever they leave. It must never collapse, however large the text is set.
TEST(the_graph_area_survives_the_largest_ui_text_scale) {
    HeadlessImGui imgui;
    UiHarness u(kFixture);

    for (float scale : {0.7f, 1.0f, 1.8f, 2.5f}) {
        u.world().resource<ecs::ViewSettings>().ui_text_scale = scale;
        ImGuiIO& io = ImGui::GetIO();
        for (int frame = 0; frame < 3; ++frame) {
            io.FontGlobalScale = scale;
            ImGui::NewFrame();
            ui::draw_panels(u.world());
            ImGui::Render();
        }
        const auto& viewport = u.world().resource<ecs::Viewport>();
        CHECK(viewport.free_size.x > 200.0f);
        CHECK(viewport.free_size.y > 200.0f);
    }
}

// The toolbar wraps rather than clipping, so it gets taller as text grows. If it
// stopped reporting its height the panels below would overlap it.
TEST(the_toolbar_reports_a_larger_height_as_text_grows) {
    HeadlessImGui imgui;
    UiHarness u(kFixture);

    auto measure_bar = [&](float scale) {
        u.world().resource<ecs::ViewSettings>().ui_text_scale = scale;
        ImGuiIO& io = ImGui::GetIO();
        for (int frame = 0; frame < 3; ++frame) {
            io.FontGlobalScale = scale;
            ImGui::NewFrame();
            ui::draw_panels(u.world());
            ImGui::Render();
        }
        return u.world().resource<ecs::Viewport>().top_bar_height;
    };

    const float small = measure_bar(1.0f);
    const float large = measure_bar(2.2f);
    CHECK(small > 0.0f);
    CHECK(large > small);
}

// -- hiding the panels ------------------------------------------------------------
//
// A view whose job is to be looked at should be able to have the window. The toolbar
// stays, because it is the way back and the controls that change what the graph shows
// live on it.

namespace {

// Drives the real panel pass and reports what the graph was left.
Vec2 free_size_with_panels(UiHarness& ui, bool shown) {
    ui.world().resource<ecs::ViewSettings>().show_panels = shown;
    ImGuiIO& io = ImGui::GetIO();
    for (int frame = 0; frame < 4; ++frame) {
        io.FontGlobalScale = 1.0f;
        ImGui::NewFrame();
        rgv::ui::draw_panels(ui.world());
        ImGui::Render();
    }
    return ui.world().resource<ecs::Viewport>().free_size;
}

} // namespace

TEST(hiding_the_panels_gives_the_graph_the_rest_of_the_window) {
    HeadlessImGui imgui;
    UiHarness     ui(kFixture);

    const Vec2 with    = free_size_with_panels(ui, true);
    const Vec2 without = free_size_with_panels(ui, false);

    CHECK(without.x > with.x);
    CHECK(without.y > with.y);

    // The whole width, and everything below the toolbar.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    CHECK(std::abs(without.x - vp->WorkSize.x) < 0.5f);
    const float top = vp->WorkSize.y - without.y;
    CHECK(top > 0.0f);                     // the toolbar keeps its band
    CHECK(top < vp->WorkSize.y * 0.26f);   // and nothing else does

    // Back again, exactly as it was: the toggle is not a one-way door.
    CHECK(close_enough(ImVec2(free_size_with_panels(ui, true).x,
                              free_size_with_panels(ui, true).y),
                       ImVec2(with.x, with.y)));
}

TEST(the_panels_are_not_drawn_at_all_when_hidden) {
    HeadlessImGui imgui;
    UiHarness     ui(kFixture);

    free_size_with_panels(ui, true);
    for (const char* name : {"Session", "Inspector", "Scenario"}) {
        CHECK(ImGui::FindWindowByName(name) != nullptr);
    }
    // ImGui keeps a window object once it has existed, so the test is whether it was
    // submitted this frame rather than whether it is remembered.
    free_size_with_panels(ui, false);
    const int frame = ImGui::GetFrameCount();
    for (const char* name : {"Session", "Inspector", "Scenario"}) {
        ImGuiWindow* w = ImGui::FindWindowByName(name);
        CHECK(w == nullptr || w->LastFrameActive < frame - 1);
    }
    // The toolbar is still there: it is the way back.
    ImGuiWindow* bar = ImGui::FindWindowByName("##topbar");
    CHECK(bar != nullptr);
    CHECK(bar->LastFrameActive >= frame - 1);
}


// -- panels push, they do not write -----------------------------------------------
//
// `ui::Ui` holds every resource by const reference except the command queue, so the
// rule is mostly a compile error now. What these add is the half a type cannot state:
// that the control actually pushes the right verb, and that the verb's owner applies
// it. Five commands -- SetViewMode, SetImpactLevel, SelectEdge, CyclePath and
// SelectScenario -- were implemented and pushed by nothing at all, because the panel
// did the work inline instead; a compiler cannot notice that.

namespace {

// One real panel frame at 1x.
void panel_frame(ecs::World& w) {
    ImGui::GetIO().FontGlobalScale = 1.0f;
    ImGui::NewFrame();
    rgv::ui::draw_panels(w);
    ImGui::Render();
}

// ImGui names a child window "Parent/##child_XXXXXXXX", so the transport controls
// cannot be found by an exact name.
ImGuiWindow* window_starting_with(const char* prefix) {
    ImGuiContext& g   = *ImGui::GetCurrentContext();
    const std::size_t n = std::strlen(prefix);
    for (ImGuiWindow* w : g.Windows) {
        if (std::strncmp(w->Name, prefix, n) == 0) return w;
    }
    return nullptr;
}

// Presses a control through the widget's own code. ImGui queues the activation and the
// item consumes it the next time it is submitted, which is the same path a click takes
// through ButtonBehavior -- so this drives the real panel, not a stand-in for it.
// Buttons and checkboxes only: a slider activates into keyboard-entry mode rather than
// producing a value.
bool press(ecs::World& w, const char* window_prefix, const char* item) {
    ImGuiWindow* win = window_starting_with(window_prefix);
    if (!win) return false;
    ImGui::ActivateItemByID(win->GetID(item));
    panel_frame(w);
    return true;
}

template <class T>
const T* queued(ecs::World& w) {
    for (const auto& c : w.resource<ecs::CommandQueue>().pending) {
        if (const T* t = std::get_if<T>(&c)) return t;
    }
    return nullptr;
}

} // namespace

TEST(a_toolbar_checkbox_asks_for_the_change_instead_of_making_it) {
    HeadlessImGui imgui;
    UiHarness     u(kFixture);
    panel_frame(u.world());   // the toolbar has to exist before it can be pressed

    CHECK(u.world().resource<ecs::ViewSettings>().show_labels);
    CHECK(press(u.world(), "##topbar", "Labels"));

    // The frame that drew the click changed nothing.
    CHECK(u.world().resource<ecs::ViewSettings>().show_labels);

    const auto* cmd = queued<ecs::SetViewToggle>(u.world());
    CHECK(cmd != nullptr);
    CHECK(cmd->which == ecs::ViewToggle::ShowLabels);
    CHECK(!cmd->on);

    // CommandSystem is the single place it lands.
    u.h.tick();
    CHECK(!u.world().resource<ecs::ViewSettings>().show_labels);
}

TEST(a_filter_checkbox_asks_for_the_change_instead_of_writing_filters) {
    HeadlessImGui imgui;
    UiHarness     u(kFixture);
    panel_frame(u.world());

    CHECK(u.world().resource<ecs::Filters>().show_unaffected);
    CHECK(press(u.world(), "##topbar", "Unaffected"));
    CHECK(u.world().resource<ecs::Filters>().show_unaffected);

    const auto* cmd = queued<ecs::SetFilterFlag>(u.world());
    CHECK(cmd != nullptr);
    CHECK(cmd->which == ecs::FilterFlag::ShowUnaffected);
    CHECK(!cmd->on);

    u.h.tick();
    CHECK(!u.world().resource<ecs::Filters>().show_unaffected);
}

// The mode switch carries three consequences -- the default level, the rebuild, the
// refit. The toolbar used to spell all three out itself; now it says one word.
TEST(choosing_a_view_mode_says_so_and_lets_one_system_decide_what_that_means) {
    HeadlessImGui imgui;
    UiHarness     u(kFixture);
    panel_frame(u.world());

    CHECK(u.world().resource<ecs::ViewSettings>().mode == ecs::ViewMode::Architecture);
    CHECK(press(u.world(), "##topbar", "Filesystem"));
    CHECK(u.world().resource<ecs::ViewSettings>().mode == ecs::ViewMode::Architecture);

    const auto* cmd = queued<ecs::SetViewMode>(u.world());
    CHECK(cmd != nullptr);
    CHECK(cmd->mode == static_cast<int>(ecs::ViewMode::Filesystem));

    u.h.tick();
    const auto& vs = u.world().resource<ecs::ViewSettings>();
    CHECK(vs.mode == ecs::ViewMode::Filesystem);
    CHECK(vs.level == ecs::default_level(ecs::ViewMode::Filesystem));
}

// The transport panel used to call play() and seek_ms() on the timeline directly,
// duplicating TransportSystem, which already owned them for the keyboard.
TEST(the_transport_buttons_ask_transport_system_rather_than_moving_the_timeline) {
    HeadlessImGui imgui;
    UiHarness     u(kFixture);
    panel_frame(u.world());

    Timeline* tl = u.source->timeline();
    CHECK(tl != nullptr);
    const bool was_playing = tl->playing();

    CHECK(press(u.world(), "Scenario/##transport", was_playing ? "Pause" : "Play"));
    CHECK(tl->playing() == was_playing);   // the panel did not touch it
    CHECK(queued<ecs::TransportPlayPause>(u.world()) != nullptr);

    u.h.tick();
    CHECK(tl->playing() != was_playing);
    // Drained by TransportSystem in Input, so CommandSystem never saw it.
    CHECK(u.world().resource<ecs::CommandQueue>().empty());
}

// The five that were dead. Each was implemented in its owning system and pushed by
// nothing, because the panel reached past the queue and did the work inline.
TEST(the_commands_the_panels_now_push_are_applied_by_their_owners) {
    HeadlessImGui imgui;
    UiHarness     u(kFixture);
    auto&         cmd = u.world().resource<ecs::CommandQueue>();

    cmd.push(ecs::SetImpactLevel{static_cast<int>(Level::File)});
    u.h.tick();
    CHECK(u.world().resource<ecs::ViewSettings>().level == Level::File);

    // An edge id from the store, so this is the real selection path rather than a
    // string round trip.
    const std::string edge_id = u.h.store().edges().begin()->first;
    cmd.push(ecs::SelectEdge{edge_id});
    u.h.tick();
    CHECK(u.world().resource<ecs::Selection>().edge == edge_id);

    cmd.push(ecs::ClearEdgeSelection{});
    u.h.tick();
    CHECK(u.world().resource<ecs::Selection>().edge.empty());

    if (u.source->scenarios().size() > 1) {
        cmd.push(ecs::SelectScenario{1});
        u.h.tick();
        CHECK(u.source->scenario_index() == 1u);
    }

    // CyclePath lands as a delta; SelectionSystem is what wraps it against the real
    // number of explanations, so with nothing selected it settles back to zero.
    cmd.push(ecs::CyclePath{+1});
    u.h.tick();
    CHECK(u.world().resource<ecs::Selection>().path_index == 0);
}

// The backstop for everything a const reference cannot reach: a const_cast, or a
// second non-const route to a resource. Draw every panel, repeatedly, and nothing the
// user chose may have moved.
TEST(drawing_every_panel_changes_no_state_at_all) {
    HeadlessImGui imgui;
    UiHarness     u(kFixture);
    u.world().resource<ecs::ViewSettings>().show_text_settings = true;
    u.world().resource<ecs::Selection>().node    = u.h.store().nodes().begin()->first;
    u.world().resource<ecs::Selection>().hovered = u.h.store().nodes().begin()->first;
    u.world().resource<ecs::Selection>().hover_time = 1.0f;

    const ecs::ViewSettings view_before    = u.world().resource<ecs::ViewSettings>();
    const ecs::Selection    selection_before = u.world().resource<ecs::Selection>();
    const auto&             f              = u.world().resource<ecs::Filters>();
    const bool  unaffected_before = f.show_unaffected;
    const int   depth_before      = f.max_impact_depth;
    const float relevance_before  = f.min_relevance;
    const std::size_t hidden_before = f.hidden.size();

    Timeline*    tl              = u.source->timeline();
    const bool   playing_before  = tl->playing();
    const double rate_before     = tl->rate();
    const double position_before = u.source->status().position_ms;

    ImGuiIO& io = ImGui::GetIO();
    for (int frame = 0; frame < 8; ++frame) {
        io.FontGlobalScale = 1.0f;
        ImGui::NewFrame();
        rgv::ui::draw_panels(u.world());
        rgv::ui::draw_graph_overlay(u.world(), ImGui::GetBackgroundDrawList());
        rgv::ui::draw_hover_card(u.world());
        rgv::ui::draw_text_settings(u.world());
        ImGui::Render();
    }

    const auto& view = u.world().resource<ecs::ViewSettings>();
    CHECK(view.mode == view_before.mode);
    CHECK(view.level == view_before.level);
    CHECK(view.show_panels == view_before.show_panels);
    CHECK(view.layout_running == view_before.layout_running);
    CHECK(view.show_labels == view_before.show_labels);
    CHECK(view.show_arrows == view_before.show_arrows);
    CHECK(view.show_text_settings == view_before.show_text_settings);
    CHECK(view.ui_text_scale == view_before.ui_text_scale);
    CHECK(view.graph_text_scale == view_before.graph_text_scale);

    const auto& selection = u.world().resource<ecs::Selection>();
    CHECK(selection.node == selection_before.node);
    CHECK(selection.edge == selection_before.edge);
    CHECK(selection.path_index == selection_before.path_index);

    CHECK(f.show_unaffected == unaffected_before);
    CHECK(f.max_impact_depth == depth_before);
    CHECK(f.min_relevance == relevance_before);
    CHECK(f.hidden.size() == hidden_before);

    CHECK(tl->playing() == playing_before);
    CHECK(tl->rate() == rate_before);
    CHECK(u.source->status().position_ms == position_before);

    // The scene is not rebuilt or re-laid-out by being looked at either.
    const auto& requests = u.world().resource<ecs::SceneRequests>();
    CHECK(!requests.rebuild);
    CHECK(!requests.relayout);
    CHECK(!requests.revisit);
    CHECK(!requests.refresh_extents);
}

// Tab used to reach into ViewSettings from NavigationSystem while F and Escape, two
// lines below it, went through the queue -- so two of the three keys on one keyboard
// were applied in one order and the third in another.
TEST(the_panels_key_goes_through_the_queue_like_every_other_key) {
    HeadlessImGui imgui;
    UiHarness     u(kFixture);

    CHECK(u.world().resource<ecs::ViewSettings>().show_panels);
    u.h.input().panels_pressed = true;
    u.h.tick();
    u.h.input().panels_pressed = false;
    CHECK(!u.world().resource<ecs::ViewSettings>().show_panels);

    // Same frame, not the next one: NavigationSystem pushes in Input and CommandSystem
    // drains in Sync, so routing it costs nothing in latency.
    u.h.input().panels_pressed = true;
    u.h.tick();
    u.h.input().panels_pressed = false;
    CHECK(u.world().resource<ecs::ViewSettings>().show_panels);
}
