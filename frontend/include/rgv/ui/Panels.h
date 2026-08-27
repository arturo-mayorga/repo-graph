// The panel layer.
//
// Everything the user reads and every control they touch. Panels never mutate the
// graph -- they mutate ViewState and set request flags, and the frame loop acts on
// them. That keeps "what the backend said" and "what the user is looking at" separable.
#pragma once

#include "rgv/ecs/LayoutSystem.h"
#include "rgv/ecs/Scene.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/render/GraphRenderer.h"

struct ImDrawList;

namespace rgv::ui {

struct UiContext {
    GraphStore&        store;
    ecs::Scene&        scene;
    ecs::LayoutSystem& layout;
    IGraphSource&      source;
    // Non-null exactly when the source is replayable. This is the only place the app
    // distinguishes a fixture from a live backend, and it does so by capability.
    fixture::FixtureSource* fixtures = nullptr;

    const render::GraphRenderer::Stats* render_stats = nullptr;
    float                               fps          = 0.0f;
    float                               frame_ms     = 0.0f;

    // Requests raised by panels, serviced by the frame loop.
    bool request_rebuild      = false;   // visible node set may have changed
    bool request_fit          = false;
    bool request_focus_impact = false;
    bool request_reload       = false;   // re-read the fixture set from disk
    bool request_extents      = false;   // text scale moved; node boxes need resizing
    bool request_save_settings = false;  // a persisted preference was committed
    int  request_fixture      = -1;      // switch to fixture set N

    // Geometry of the area not covered by panels.
    Vec2 free_origin{0.0f, 0.0f};
    Vec2 free_size{0.0f, 0.0f};

    // The toolbar wraps its controls, so its height depends on how many rows they
    // needed. Measured while drawing and fed back in on the next frame, because the
    // window has to be sized before its contents are known.
    float top_bar_height  = 64.0f;   // in:  last frame's measurement
    float measured_top_bar = 64.0f;  // out: this frame's
};

// Draws every panel and updates ctx.free_* to the leftover viewport rect.
void draw_panels(UiContext& ctx, const std::vector<std::string>& fixture_names,
                 int current_fixture);

// Node labels, impact badges, and the legend, drawn over the GL scene through ImGui's
// background draw list. Text lives here rather than in the renderer because a glyph
// atlas is a subsystem and ImGui already has one.
void draw_graph_overlay(UiContext& ctx, ImDrawList* draw);

// The card that fades in under the pointer after a short dwell. At overview zoom,
// where labels are gone, this is the only thing that names a node -- so it carries
// more than a label would: change state, impact distance, and the reason chain.
void draw_hover_card(UiContext& ctx);

// The text-size window. Its own layout is deliberately immune to the scale it edits:
// a control that resizes itself while being dragged cannot be aimed.
void draw_text_settings(UiContext& ctx);

} // namespace rgv::ui
