// Panel layout invariants, driven through real ImGui frames with no window and no GPU.
//
// These exist because of a specific, easy-to-reintroduce bug: a control that edits the
// UI text scale, while being scaled by it, resizes and moves under the cursor as you
// drag it, making a particular value impossible to hit. The fix is that the text-size
// window's own geometry is independent of the scale it edits, and that is exactly what
// is asserted here.
#include "TestMain.h"

#include "Harness.h"

#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/ui/Panels.h"
#include "rgv/ui/Theme.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <memory>

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
