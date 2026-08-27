// rgv -- Live Repository Impact Graph, native frontend.
//
// The frame loop is the whole architecture in one function:
//
//     source.poll(dt, store)     -- events arrive (fixture replay today, WS later)
//     scene.sync(store)          -- ECS is patched from the store's dirty set
//     layout.step(scene, dt)     -- only what moved is relaxed
//     renderer -> GL             -- three instanced draw calls
//     panels  -> ImGui           -- everything the user reads and touches
//
// Nothing below the source knows whether the events came from a file or a socket.

#include "rgv/config/Settings.h"
#include "rgv/ecs/LayoutSystem.h"
#include "rgv/ecs/Scene.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/render/GL.h"
#include "rgv/render/GraphRenderer.h"
#include "rgv/ui/Panels.h"
#include "rgv/ui/Theme.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef RGV_FIXTURE_DIR
#define RGV_FIXTURE_DIR "fixtures"
#endif

namespace fs = std::filesystem;
using namespace rgv;

namespace {

struct App {
    GraphStore        store;
    ecs::Scene        scene;
    ecs::LayoutSystem layout;
    render::GraphRenderer renderer;

    std::vector<std::string> fixture_dirs;
    std::vector<std::string> fixture_names;
    int                      current_fixture = 0;
    std::unique_ptr<fixture::FixtureSource> source;

    float                 top_bar_height = 64.0f;
    float                 time_s         = 0.0f;

    config::Settings      settings;
    std::filesystem::path settings_file;

    // interaction
    entt::entity dragging  = entt::null;
    bool         panning   = false;
    Vec2         last_mouse{0, 0};
    bool         first_frame = true;
};

void glfw_error(int code, const char* desc) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

std::vector<std::string> discover_fixtures(const std::string& root) {
    std::vector<std::string> out;
    std::error_code          ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (e.is_directory() && fs::exists(e.path() / "snapshot.json")) {
            out.push_back(e.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool load_fixture(App& app, int index) {
    if (index < 0 || index >= static_cast<int>(app.fixture_dirs.size())) return false;
    try {
        auto set = fixture::FixtureSet::load(app.fixture_dirs[static_cast<std::size_t>(index)]);
        app.source          = std::make_unique<fixture::FixtureSource>(std::move(set), 0);
        app.current_fixture = index;
        app.scene.view.selected_node.clear();
        app.scene.view.selected_edge.clear();
        return true;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "fixture load failed: %s\n", ex.what());
        return false;
    }
}

// Everything the mouse and keyboard do to the graph. Kept out of the panels so that
// input handling cannot accidentally depend on panel draw order.
void handle_input(App& app, GLFWwindow* window, const ui::UiContext& ui, float dt) {
    ImGuiIO& io   = ImGui::GetIO();
    auto&    view = app.scene.view;
    auto&    reg  = app.scene.registry;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    // Framebuffer may be scaled relative to window coordinates on hidpi displays.
    int ww = 1, wh = 1, fw = 1, fh = 1;
    glfwGetWindowSize(window, &ww, &wh);
    glfwGetFramebufferSize(window, &fw, &fh);
    const float sx = static_cast<float>(fw) / std::max(1, ww);
    const float sy = static_cast<float>(fh) / std::max(1, wh);
    const Vec2  mouse{static_cast<float>(mx) * sx, static_cast<float>(my) * sy};

    const bool over_free =
        mouse.x >= ui.free_origin.x * sx && mouse.x <= (ui.free_origin.x + ui.free_size.x) * sx &&
        mouse.y >= ui.free_origin.y * sy && mouse.y <= (ui.free_origin.y + ui.free_size.y) * sy;
    const bool mouse_free = !io.WantCaptureMouse && over_free;

    // -- zoom about the cursor, so the thing under the pointer stays under it
    if (mouse_free && io.MouseWheel != 0.0f) {
        const Vec2  before = view.camera.screen_to_world(mouse);
        const float factor = std::pow(1.14f, io.MouseWheel);
        view.camera.zoom   = std::clamp(view.camera.zoom * factor, 0.02f, 6.0f);
        view.auto_fit      = false;
        const Vec2 after   = view.camera.screen_to_world(mouse);
        view.camera.center += before - after;
    }

    // -- press
    if (mouse_free && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const entt::entity hit = app.scene.pick(mouse);
        if (hit != entt::null) {
            if (const auto* ref = reg.try_get<ecs::NodeRef>(hit)) {
                view.selected_node = ref->id;
                view.selected_edge.clear();
                view.path_index = 0;
                app.scene.update_explained_path(app.store);
                reg.clear<ecs::Selected>();
                reg.emplace_or_replace<ecs::Selected>(hit);
                app.scene.restyle(app.store);
            }
            app.dragging = hit;
        } else {
            app.panning = true;
        }
        app.last_mouse = mouse;
    }

    if (mouse_free && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const entt::entity hit = app.scene.pick(mouse);
        if (hit != entt::null) {
            // Pinning is how a user says "this one stays where I put it" while the
            // rest of the layout keeps relaxing around it.
            if (reg.all_of<ecs::Pinned>(hit)) reg.remove<ecs::Pinned>(hit);
            else reg.emplace<ecs::Pinned>(hit);
        }
    }

    // -- drag
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const Vec2 delta = mouse - app.last_mouse;
        if (app.dragging != entt::null) {
            if (auto* pos = reg.try_get<ecs::Position>(app.dragging)) {
                pos->p += delta / view.camera.zoom;
                reg.emplace_or_replace<ecs::Pinned>(app.dragging);
                if (auto* vel = reg.try_get<ecs::Velocity>(app.dragging)) vel->v = Vec2{0, 0};
            }
        } else if (app.panning) {
            view.camera.center -= delta / view.camera.zoom;
            if (length_sq(delta) > 0.0f) view.auto_fit = false;
        }
        app.last_mouse = mouse;
    } else {
        app.dragging = entt::null;
        app.panning  = false;
    }

    // -- hover. The dwell timer is what gates the card: it only appears once the
    //    pointer has actually settled on something, not while sweeping past it.
    if (mouse_free) {
        reg.clear<ecs::Hovered>();
        const entt::entity hit = app.scene.pick(mouse);
        NodeId             id;
        if (hit != entt::null) {
            reg.emplace_or_replace<ecs::Hovered>(hit);
            if (const auto* ref = reg.try_get<ecs::NodeRef>(hit)) id = ref->id;
        }
        if (id == view.hovered_node && !id.empty()) {
            view.hover_time += dt;
        } else {
            view.hovered_node = id;
            view.hover_time   = 0.0f;
        }
        // Dragging or panning is not dwelling; the card would just follow the cursor.
        if (app.dragging != entt::null || app.panning) view.hover_time = 0.0f;
    }
    // When the pointer is outside the graph area the hover state is simply left as it
    // is, so a card forced on by --hover survives for a screenshot and a real hover
    // does not blink out while the user reaches for a panel control.

    if (io.WantCaptureKeyboard) return;

    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        app.scene.focus_on({});
        view.auto_fit = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        view.selected_node.clear();
        view.selected_edge.clear();
        reg.clear<ecs::Selected>();
        app.scene.restyle(app.store);
    }
    if (Timeline* tl = app.source ? app.source->timeline() : nullptr) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            tl->playing() ? tl->pause() : tl->play();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Period, false)) tl->step_event();
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tl->restart();
    }
}

// Walks the ECS and hands the renderer its instance list. The only place that turns
// components into geometry.
void submit_scene(App& app) {
    auto&       reg  = app.scene.registry;
    const auto& view = app.scene.view;
    const auto& t    = ui::theme();

    app.renderer.begin(view.camera, t.background);

    // One semantic-zoom decision for the whole frame. Nodes collapse to constant-size
    // dots below the point where a label would be legible; picking uses the same
    // function, so what is clickable is exactly what is drawn.
    const ecs::NodeDetail detail = ecs::node_detail(view);

    auto emphasised = [&](entt::entity e) {
        return reg.all_of<ecs::Selected>(e) || reg.all_of<ecs::Hovered>(e) ||
               reg.all_of<ecs::OnExplainedPath>(e);
    };
    auto half_of = [&](entt::entity e) -> Vec2 {
        const auto* ext = reg.try_get<ecs::Extent>(e);
        if (!ext) return Vec2{6.0f, 6.0f};
        return ecs::render_half(view, detail, ext->half,
                                ecs::dot_px_for(reg.all_of<ecs::Changed>(e),
                                                reg.all_of<ecs::Impacted>(e), emphasised(e)));
    };

    // Edges first, and the explained path last within that pass, so the explanation
    // is never buried under the graph it is explaining.
    auto emit_edge = [&](entt::entity ent, const ecs::Endpoints& ends, const ecs::Style& style,
                         bool on_path) {
        const entt::entity ea_ent = static_cast<entt::entity>(ends.from);
        const entt::entity eb_ent = static_cast<entt::entity>(ends.to);
        const auto*        pa     = reg.try_get<ecs::Position>(ea_ent);
        const auto*        pb     = reg.try_get<ecs::Position>(eb_ent);
        if (!pa || !pb) return;

        const Vec2 dir = normalize(pb->p - pa->p);
        if (length_sq(dir) < 1e-6f) return;

        // Stop the line at the node boundary rather than the centre, so the arrowhead
        // lands on the edge of the shape at any zoom level.
        auto edge_point = [](const Vec2& c, const Vec2& half, const Vec2& d) {
            const float tx = d.x != 0.0f ? half.x / std::abs(d.x) : 1e9f;
            const float ty = d.y != 0.0f ? half.y / std::abs(d.y) : 1e9f;
            return c + d * std::min(tx, ty);
        };
        const Vec2 a = edge_point(pa->p, half_of(ea_ent), dir);
        const Vec2 b = edge_point(pb->p, half_of(eb_ent), dir * -1.0f);

        Vec4  color = style.stroke;
        float width = style.stroke_w;
        if (on_path) {
            color = t.path;
            width = std::max(width, 3.2f);
        } else {
            // Context recedes further as the view zooms out: at overview scale the
            // edges are what turn a readable graph into a hairball.
            color.a *= (0.35f + 0.65f * style.emphasis) * (0.45f + 0.55f * detail.t);
        }
        app.renderer.add_edge(a, b, color, width, style.dash);
        if (view.show_arrows && detail.t > 0.15f) {
            app.renderer.add_arrow(b, dir, on_path ? 13.0f : 9.0f, color);
        }
    };

    for (auto [ent, ends, style] : reg.view<const ecs::Endpoints, const ecs::Style>().each()) {
        if (!reg.all_of<ecs::OnExplainedPath>(ent)) emit_edge(ent, ends, style, false);
    }
    for (auto [ent, ends, style, on_path] :
         reg.view<const ecs::Endpoints, const ecs::Style, const ecs::OnExplainedPath>().each()) {
        emit_edge(ent, ends, style, true);
    }

    for (auto [ent, pos, ext, style, ref] :
         reg.view<const ecs::Position, const ecs::Extent, const ecs::Style, const ecs::NodeRef>()
             .each()) {
        Vec4  fill   = style.fill;
        Vec4  stroke = style.stroke;
        float sw     = style.stroke_w;

        const bool changed = reg.all_of<ecs::Changed>(ent);
        const Vec2 half    = half_of(ent);
        // Fully round when collapsed to a dot, a soft rectangle when the label is up.
        const float radius = 5.0f * detail.t + std::min(half.x, half.y) * (1.0f - detail.t);

        // A seed gets a halo: at a glance, "the agent touched this" must be findable
        // without reading a single label -- which is the only cue left at dot scale.
        if (changed) {
            const float glow = 9.0f * detail.t + 5.0f * (1.0f - detail.t);
            app.renderer.add_node(pos.p, half + Vec2{glow, glow}, t.seed_glow,
                                  Vec4{0, 0, 0, 0}, 0.0f, 0.0f, radius + glow);
        }

        // A changed hub screams. Expanding rings, because a static halo is exactly
        // what every other changed node already has -- and this is categorically
        // different: most of the repository depends on what just moved.
        if (const auto* hub = reg.try_get<ecs::HubSeed>(ent)) {
            const float reach = std::clamp(hub->reach_fraction, 0.25f, 1.0f);
            for (int ring = 0; ring < 3; ++ring) {
                const float phase = std::fmod(app.time_s * 0.75f + ring / 3.0f, 1.0f);
                const float grow  = (12.0f + phase * 62.0f * reach) / std::max(view.camera.zoom, 0.05f);
                Vec4        c     = t.changed;
                // Fade as it expands, so the rings read as emanating rather than blinking.
                c.a = 0.55f * (1.0f - phase) * (1.0f - phase);
                app.renderer.add_node(pos.p, half + Vec2{grow, grow}, Vec4{0, 0, 0, 0}, c,
                                      2.2f, 0.0f, radius + grow);
            }
        }
        if (reg.all_of<ecs::OnExplainedPath>(ent)) {
            stroke = t.path;
            sw     = std::max(sw, 3.0f);
        }
        if (reg.all_of<ecs::Hovered>(ent)) {
            stroke = t.hover;
            sw     = std::max(sw, 2.6f);
        }
        if (reg.all_of<ecs::Selected>(ent)) {
            stroke = t.selection;
            sw     = 3.4f;
        }
        if (reg.all_of<ecs::Pinned>(ent)) fill = mix(fill, t.direct, 0.10f);

        // A dot is mostly outline; without a lift in fill it reads as a hollow ring
        // against the background.
        if (detail.t < 0.5f) fill = mix(stroke, fill, 0.35f + 0.65f * detail.t * 2.0f);

        app.renderer.add_node(pos.p, half, fill, stroke, sw, style.dash, radius);
    }

    app.renderer.flush();
}

} // namespace

int main(int argc, char** argv) {
    App app;

    std::string root = RGV_FIXTURE_DIR;
    std::string want_fixture;
    int         want_scenario = 0;
    bool        want_fullscreen = false;
    std::string want_select;
    std::string want_hover;
    bool        want_text_settings = false;
    float       want_relevance     = -1.0f;
    double      want_at = -1.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            std::cout << "usage: rgv [options] [fixture-root]\n"
                         "  --fixture NAME   start on this fixture set\n"
                         "  --scenario N     start on scenario N\n"
                         "  --fullscreen     open fullscreen on the primary monitor\n"
                         "  --at MS          seek to this point in the scenario and pause\n"
                         "  --select NODE    select this node id on startup\n"
                         "  --hover NODE     show this node's hover card on startup\n"
                         "  --text-settings  open the text size window on startup\n"
                         "  --relevance F    start with the relevance filter at F (0..1)\n"
                         "  fixture-root     directory of fixture sets "
                         "(default: " RGV_FIXTURE_DIR ")\n";
            return 0;
        }
        if (a == "--fullscreen") want_fullscreen = true;
        else if (a == "--at" && i + 1 < argc) want_at = std::atof(argv[++i]);
        else if (a == "--select" && i + 1 < argc) want_select = argv[++i];
        else if (a == "--hover" && i + 1 < argc) want_hover = argv[++i];
        else if (a == "--text-settings") want_text_settings = true;
        else if (a == "--relevance" && i + 1 < argc) want_relevance = std::atof(argv[++i]);
        else if (a == "--fixture" && i + 1 < argc) want_fixture = argv[++i];
        else if (a == "--scenario" && i + 1 < argc) want_scenario = std::atoi(argv[++i]);
        else root = a;
    }

    app.fixture_dirs = discover_fixtures(root);
    if (app.fixture_dirs.empty()) {
        std::fprintf(stderr,
                     "no fixture sets found under %s\n"
                     "expected subdirectories each containing snapshot.json\n",
                     root.c_str());
        return 1;
    }
    for (const auto& d : app.fixture_dirs) {
        app.fixture_names.push_back(fs::path(d).filename().string());
    }

    // Start on the smallest fixture unless told otherwise. Opening on a 240-package
    // stress graph teaches nothing about the interaction; opening on the readable one
    // does, and the scale probe is one dropdown away.
    int initial = 0;
    if (!want_fixture.empty()) {
        for (std::size_t i = 0; i < app.fixture_names.size(); ++i) {
            if (app.fixture_names[i] == want_fixture) initial = static_cast<int>(i);
        }
    } else {
        std::uintmax_t smallest = 0;
        for (std::size_t i = 0; i < app.fixture_dirs.size(); ++i) {
            std::error_code ec;
            const auto sz = fs::file_size(fs::path(app.fixture_dirs[i]) / "snapshot.json", ec);
            if (ec) continue;
            if (smallest == 0 || sz < smallest) { smallest = sz; initial = static_cast<int>(i); }
        }
    }
    // Preferences load before anything is drawn, so the first frame is already at the
    // user's text size rather than snapping to it a frame later.
    app.settings_file          = config::settings_path();
    app.settings               = config::load(app.settings_file);
    app.scene.view.ui_text_scale    = app.settings.ui_text_scale;
    app.scene.view.graph_text_scale = app.settings.graph_text_scale;


    if (want_relevance >= 0.0f) {
        app.scene.view.filters.min_relevance = std::clamp(want_relevance, 0.0f, 1.0f);
    }

    if (!load_fixture(app, initial)) return 1;
    if (want_scenario > 0) app.source->select_scenario(static_cast<std::size_t>(want_scenario));

    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    // Real fullscreen through GLFW rather than a window-manager hint: demoing the
    // interaction should not depend on which compositor is running.
    GLFWmonitor*       monitor = want_fullscreen ? glfwGetPrimaryMonitor() : nullptr;
    const GLFWvidmode* mode    = monitor ? glfwGetVideoMode(monitor) : nullptr;
    const int          win_w   = mode ? mode->width : 1680;
    const int          win_h   = mode ? mode->height : 1000;
    if (mode) {
        glfwWindowHint(GLFW_RED_BITS, mode->redBits);
        glfwWindowHint(GLFW_GREEN_BITS, mode->greenBits);
        glfwWindowHint(GLFW_BLUE_BITS, mode->blueBits);
        glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
    }

    GLFWwindow* window = glfwCreateWindow(win_w, win_h, "rgv - live repository impact graph",
                                          monitor, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create a GL 3.3 core context\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gl::load(reinterpret_cast<gl::ProcLoader>(glfwGetProcAddress))) {
        std::fprintf(stderr, "missing OpenGL entry points: %s\n", gl::missing());
        return 1;
    }
    std::printf("GL %s | %s\n", reinterpret_cast<const char*>(gl::glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(gl::glGetString(GL_RENDERER)));
    std::fflush(stdout);

    std::string err;
    if (!app.renderer.init(&err)) {
        std::fprintf(stderr, "renderer init failed:\n%s\n", err.c_str());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // panel geometry is code, not user state
    ui::apply_imgui_style();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    double      pending_at     = want_at;
    std::string pending_select = want_select;
    std::string pending_hover  = want_hover;
    // Opening the text window is deferred like the other startup state: its position
    // depends on the toolbar's measured height, which is not known on frame one.
    bool pending_text_settings = want_text_settings;

    double last = glfwGetTime();
    float  fps_smoothed = 60.0f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        app.time_s       = static_cast<float>(now);
        const float  dt  = static_cast<float>(std::min(0.1, now - last));
        last             = now;
        fps_smoothed     = fps_smoothed * 0.92f + (dt > 0.0f ? 1.0f / dt : 60.0f) * 0.08f;

        int fw = 0, fh = 0;
        glfwGetFramebufferSize(window, &fw, &fh);
        app.scene.view.camera.vw = static_cast<float>(std::max(1, fw));
        app.scene.view.camera.vh = static_cast<float>(std::max(1, fh));

        // --- 1. events in
        if (app.source->take_reset()) {
            app.store.reset(app.source->baseline());
            app.scene.rebuild(app.store);
            app.layout.reset(app.scene, app.store);
            app.scene.focus_on({});
        }
        app.source->poll(dt, app.store);

        // --- 2. store -> ECS, incrementally
        if (app.store.dirty().any()) {
            app.scene.sync(app.store);
            app.store.clear_dirty();
        }
        if (app.scene.needs_layout_reset) app.layout.reset(app.scene, app.store);

        // --- 3. ease toward the layout targets, and keep the graph framed until it
        //        stops moving. Fitting once on frame 1 is not enough: layout is still
        //        animating, and the graph would drift off screen.
        app.layout.step(app.scene, dt);
        if (app.scene.view.auto_fit && !app.layout.settled()) app.scene.focus_on({});

        // Panel chrome scales with the same preference as the graph labels, so "text
        // size" means one thing everywhere.
        ImGui::GetIO().FontGlobalScale = app.scene.view.ui_text_scale;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ui::UiContext ui_ctx{app.store,   app.scene, app.layout,
                             *app.source, app.source.get()};
        ui_ctx.render_stats   = &app.renderer.stats();
        ui_ctx.top_bar_height = app.top_bar_height;
        ui_ctx.fps          = fps_smoothed;
        ui_ctx.frame_ms     = dt * 1000.0f;

        ui::draw_panels(ui_ctx, app.fixture_names, app.current_fixture);
        app.top_bar_height = ui_ctx.measured_top_bar;

        // The camera is anchored on the free area, not the window, so "fit" frames the
        // graph in the space the panels leave behind.
        app.scene.view.free_size = ui_ctx.free_size;
        app.scene.view.camera.anchor =
            ui_ctx.free_origin + ui_ctx.free_size * 0.5f;

        handle_input(app, window, ui_ctx, dt);

        if (ui_ctx.request_fixture >= 0 && ui_ctx.request_fixture != app.current_fixture) {
            load_fixture(app, ui_ctx.request_fixture);
        }
        if (ui_ctx.request_reload) load_fixture(app, app.current_fixture);
        if (ui_ctx.request_extents) {
            // Resize boxes in place rather than rebuilding: a rebuild would discard
            // every position and reshuffle the graph under the user's slider.
            app.scene.refresh_extents();
            app.layout.reset(app.scene, app.store);
        }
        if (ui_ctx.request_save_settings) {
            app.settings.ui_text_scale    = app.scene.view.ui_text_scale;
            app.settings.graph_text_scale = app.scene.view.graph_text_scale;
            app.settings.sanitize();
            if (!config::save(app.settings, app.settings_file)) {
                std::fprintf(stderr, "could not write %s\n", app.settings_file.c_str());
            }
        }
        if (ui_ctx.request_rebuild) {
            app.scene.rebuild(app.store);
            app.layout.reset(app.scene, app.store);
        }
        if (ui_ctx.request_fit) {
            app.scene.focus_on({});
            app.scene.view.auto_fit = true;
        }
        if (ui_ctx.request_focus_impact) {
            std::vector<NodeId> ids;
            if (const ImpactResult* r = app.store.impact(app.scene.view.level)) {
                for (const auto& n : r->impacted_nodes) ids.push_back(n.node_id);
            }
            app.scene.focus_on(ids);
            app.scene.view.auto_fit = false;   // the user asked for this framing
        }
        if (app.first_frame) {
            app.scene.focus_on({});
            app.first_frame = false;
        }
        // Deferred startup state. Applied after the first sync so the nodes it refers
        // to exist; this is what makes a demo or a screenshot reproducible.
        if (pending_at >= 0.0 && app.layout.settled()) {
            if (Timeline* tl = app.source->timeline()) {
                tl->pause();
                tl->seek_ms(pending_at);
            }
            pending_at = -1.0;
        }
        if (!pending_select.empty() && app.store.node(pending_select)) {
            app.scene.view.selected_node = pending_select;
            app.scene.view.path_index    = 0;
            if (const entt::entity e = app.scene.find_node(pending_select); e != entt::null) {
                app.scene.registry.clear<ecs::Selected>();
                app.scene.registry.emplace_or_replace<ecs::Selected>(e);
            }
            app.scene.update_explained_path(app.store);
            app.scene.restyle(app.store);
            pending_select.clear();
        }
        if (pending_text_settings && app.layout.settled()) {
            app.scene.view.show_text_settings = true;
            pending_text_settings             = false;
        }
        if (!pending_hover.empty() && app.store.node(pending_hover) &&
            app.scene.find_node(pending_hover) != entt::null && app.layout.settled()) {
            app.scene.view.hovered_node = pending_hover;
            app.scene.view.hover_time   = 10.0f;   // past the dwell delay, fully faded in
            pending_hover.clear();
        }
        if (ui_ctx.request_rebuild || ui_ctx.request_reload) app.scene.view.auto_fit = true;

        // --- 4. draw
        submit_scene(app);
        ui::draw_graph_overlay(ui_ctx, ImGui::GetBackgroundDrawList());
        ui::draw_hover_card(ui_ctx);
        ui::draw_text_settings(ui_ctx);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    app.renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
