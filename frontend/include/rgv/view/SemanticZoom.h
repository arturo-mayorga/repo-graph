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
    bool  labels  = true;
};

NodeDetail node_detail(float zoom, float graph_text_scale);

// Screen-space dot size, larger for what the user is meant to notice. At overview zoom
// the blast radius should read as a constellation, not a uniform mesh.
float dot_px_for(bool changed, bool impacted, bool emphasised);

// Rendered half-extent. `layout_half` is the footprint layout reserved, which never
// changes with zoom; the drawn size collapses toward a dot of `dot_px` screen pixels.
Vec2 render_half(float zoom, const NodeDetail& detail, const Vec2& layout_half, float dot_px);

// The box a node needs to hold its label at this text scale.
Vec2 text_extent(NodeKind kind, const std::string& name, const std::string& sub,
                 float graph_text_scale);

} // namespace rgv::view
