// The panel layer.
//
// Everything the user reads and every control they touch. Panels never mutate the
// graph and never write components -- they read resources and push commands. That is
// what keeps "what the backend said", "what the user is looking at", and "what the
// user just asked for" three separable things.
#pragma once

#include "rgv/ecs/World.h"

struct ImDrawList;

namespace rgv::ui {

// Draws the toolbar, session, inspector, and scenario panels, and updates the
// Viewport resource with the rectangle they leave for the graph.
void draw_panels(ecs::World& world);

// Node labels, impact badges, and the legend, over the GL scene via ImGui's draw list.
// Text lives here rather than in the renderer because a glyph atlas is a subsystem and
// ImGui already ships one.
void draw_graph_overlay(ecs::World& world, ImDrawList* draw);

// The card that fades in under the pointer after a short dwell. At overview zoom, where
// labels are gone, it is the only thing that names a node.
void draw_hover_card(ecs::World& world);

// The text-size window. Its own layout is deliberately immune to the scale it edits: a
// control that resizes itself while being dragged cannot be aimed.
void draw_text_settings(ecs::World& world);

} // namespace rgv::ui
