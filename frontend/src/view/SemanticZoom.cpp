#include "rgv/view/SemanticZoom.h"

#include <algorithm>
#include <cmath>

namespace rgv::view {

NodeDetail node_detail(float zoom, float graph_text_scale) {
    NodeDetail d;
    // Labels are drawn in world space, so their on-screen size is the base font times
    // the user's scale times zoom. That single number decides how big the text is,
    // whether it is worth drawing, and whether the node is a box or a dot.
    d.font_px = kBaseFontPx * graph_text_scale * zoom;

    // Below ~6px nothing is legible; by ~11px a two-line label is comfortable.
    constexpr float lo = 6.0f, hi = 11.0f;
    const float     x  = std::clamp((d.font_px - lo) / (hi - lo), 0.0f, 1.0f);
    d.t      = x * x * (3.0f - 2.0f * x);   // smoothstep, so the transition is not a pop
    d.labels = d.t > 0.02f;
    return d;
}

float dot_px_for(bool changed, bool impacted, bool emphasised) {
    float r = 4.5f;
    if (impacted) r = 6.0f;
    if (changed) r = 7.5f;
    if (emphasised) r += 2.0f;   // selected or hovered stays findable at any zoom
    return r;
}

Vec2 render_half(float zoom, const NodeDetail& detail, const Vec2& layout_half, float dot_px) {
    // A dot holds a constant screen size, so an overview stays a readable constellation
    // instead of fading to nothing as the user zooms out.
    const float z = std::max(zoom, 1e-4f);
    const Vec2  dot{dot_px / z, dot_px / z};
    return lerp(dot, layout_half, detail.t);
}

Vec2 text_extent(NodeKind kind, const std::string& name, const std::string& sub,
                 float graph_text_scale) {
    // ImGui's default font is a fixed-advance bitmap face, so a character-count
    // estimate matches CalcTextSize closely enough to size a box -- and it keeps this
    // free of any dependency on the UI toolkit.
    const float       advance = kBaseFontPx * graph_text_scale * kCharAdvanceRatio;
    const std::size_t widest  = std::max(name.size(), sub.size());
    const float       text_w  = static_cast<float>(widest) * advance;

    float min_w = 92.0f, h = 16.0f;
    switch (kind) {
        case NodeKind::Package:
        case NodeKind::BuildTarget:     min_w = 124.0f; h = 21.0f; break;
        case NodeKind::ExternalPackage: min_w = 104.0f; h = 17.0f; break;
        case NodeKind::Directory:       min_w = 96.0f;  h = 16.0f; break;
        default:                        break;
    }
    // Two text lines stack inside the box when zoomed in, so the height has to carry
    // the secondary line as well as the name.
    return Vec2{std::max(min_w * graph_text_scale, text_w + 26.0f * graph_text_scale) * 0.5f,
                h * graph_text_scale};
}

} // namespace rgv::view
