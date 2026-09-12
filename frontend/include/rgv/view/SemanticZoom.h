// Semantic zoom: how much of a node is worth drawing at the current scale.
//
// Below the point where a label is readable there is no value in drawing a labelled
// box -- the node becomes a small constant-size dot and the hover card takes over
// naming it. One number decides all of it: the on-screen size of a label.
//
// Rendering and picking both call these, which is the point. If they computed size
// separately, clicks would land on boxes that are no longer being drawn.
#pragma once

#include "rgv/contract/Types.h"
#include "rgv/render/Math.h"

#include <string>

namespace rgv::view {

// ImGui's default font. A constant here because node geometry is derived from text
// metrics, and the ECS must not depend on the UI toolkit to know them.
inline constexpr float kBaseFontPx       = 13.0f;
inline constexpr float kCharAdvanceRatio = 0.55f;   // advance / font size, ProggyClean

struct NodeDetail {
    float t       = 1.0f;   // 0 = compact dot, 1 = full labelled box
    float font_px = kBaseFontPx;
};

NodeDetail node_detail(float zoom, float graph_text_scale);

// Screen-space dot size, larger for what the user is meant to notice. At overview zoom
// the blast radius should read as a constellation, not a uniform mesh.
float dot_px_for(bool changed, bool impacted, bool emphasised,
                 float prominence = 1.0f);

// Rendered half-extent. `layout_half` is the footprint layout reserved, which never
// changes with zoom; the drawn size collapses toward a dot of `dot_px` screen pixels.
Vec2 render_half(float zoom, const NodeDetail& detail, const Vec2& layout_half, float dot_px);

// Half-extent for a disc. Unlike a box, a disc keeps its relative size at every zoom:
// the difference between a directory holding forty files and one holding two is
// information, and collapsing both to a uniform dot would throw it away. Only the
// floor is in screen space, so nothing vanishes when zoomed out.
Vec2 disc_half(float zoom, float world_radius, float min_px = 1.6f);

// The drawn half-extent of a node, whichever shape the view uses. Rendering and
// picking both call this, so a click can never land on something that is not drawn.
// The collapsed shape of a node. A disc view supplies a world-space radius; a box view
// leaves it zero and collapses to a constant-size dot instead. `room` is the distance to
// the nearest neighbour and gates how far the node may grow toward its label box: where
// the packing is dense -- files on an orbit are ~18 units apart while a filename box is
// ~120 wide -- it stays collapsed and its name is drawn outside.
struct DiscShape {
    float radius = 0.0f;    // 0 => collapses to a dot rather than a world-space disc
    float room   = 1e9f;
};

Vec2 node_half(float zoom, const NodeDetail& detail, const Vec2& layout_half,
               const DiscShape& shape, float dot_px);

// How far a disc has morphed toward its label box, in [0, 1]. Rendering needs it for
// the corner radius -- a circle is just a box whose corners are its own radius -- and
// using the plain zoom curve there draws rounded squares where circles belong.
float disc_morph(const NodeDetail& detail, const DiscShape& shape, const Vec2& layout_half);

// True once a node is enough of a box that its name belongs inside it. Deliberately not
// "has it grown big enough to fit the text": mid-morph that leaves a rectangle drawn
// with its name floating outside, which is the worst of both. Past this point the label
// goes in and is shrunk to fit whatever the box currently is.
bool label_belongs_inside(float morph);

// The box a node needs to hold its label at this text scale.
Vec2 text_extent(NodeKind kind, const std::string& name, const std::string& sub,
                 float graph_text_scale);

} // namespace rgv::view
