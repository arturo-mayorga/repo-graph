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

float dot_px_for(bool changed, bool impacted, bool emphasised, float prominence) {
    // The same golden-ratio scale the discs use: context, impacted, changed are r,
    // r*phi, r*phi^2. Three sizes on one geometric progression read as a family, and
    // what the eye should go to is unmistakably the largest.
    constexpr float kGolden = 1.6180339887f;
    constexpr float kBase   = 4.2f;

    float step = 1.0f;
    if (impacted) step = kGolden;
    if (changed) step = kGolden * kGolden;

    // Prominence rides the same ladder, so the two are combined by taking the larger
    // rather than by multiplying: a changed hub is the loudest thing on screen either
    // way, and phi^2 * phi^2 would make it seven times a leaf and swamp the picture.
    float r = kBase * std::max(step, prominence);
    if (emphasised) r *= 1.25f;   // selected or hovered stays findable at any zoom
    return r;
}

Vec2 render_half(float zoom, const NodeDetail& detail, const Vec2& layout_half, float dot_px) {
    // A dot holds a constant screen size, so an overview stays a readable constellation
    // instead of fading to nothing as the user zooms out.
    const float z = std::max(zoom, 1e-4f);
    const Vec2  dot{dot_px / z, dot_px / z};
    return lerp(dot, layout_half, detail.t);
}

Vec2 disc_half(float zoom, float world_radius, float min_px) {
    const float z = std::max(zoom, 1e-4f);
    const float r = std::max(world_radius, min_px / z);
    return Vec2{r, r};
}

float disc_morph(const NodeDetail& detail, const DiscShape& shape, const Vec2& layout_half) {
    // The bar for "has room" is lower than the full label width, because a name inside
    // a box is shrunk to fit it. A node only needs enough space for a legible box, not
    // for its name at full size -- otherwise anything with a long name never morphs at
    // all, however far you zoom.
    const float need = std::max(layout_half.x, 1.0f);
    const float room = std::clamp((shape.room - need * 0.30f) / (need * 0.35f), 0.0f, 1.0f);

    // The zoom half of the gate differs by view, and for a reason rather than by
    // accident. A layered layout reserves each node's label box as its footprint, so
    // the moment the label is legible the box is the right thing to draw -- that is the
    // legibility curve, which saturates near the default fit. A radial layout reserves
    // no such thing, so boxes there are something you zoom in to get; reusing the early
    // curve turns its overview into squashed boxes the moment it opens, when it should
    // be a constellation of circles.
    if (shape.radius <= 0.0f) return room * detail.t;

    const float x = std::clamp((detail.font_px - 18.0f) / 12.0f, 0.0f, 1.0f);
    return room * (x * x * (3.0f - 2.0f * x));
}

Vec2 node_half(float zoom, const NodeDetail& detail, const Vec2& layout_half,
               const DiscShape& shape, float dot_px) {
    // Two gates, multiplied, and the same two in every view:
    //
    //   room  -- is there space between this node and its neighbours for a box? In the
    //            radial view the answer is yes for the repository and its packages, and
    //            no for files on an orbit, which sit ~18 units apart while a filename
    //            box is ~120 wide. Those stay collapsed and are named from outside.
    //   zoom  -- has the user zoomed past an overview? The part they drive.
    //
    // The collapsed shape differs by view: a disc view supplies a world-space radius
    // whose size means something, a box view collapses to a constant-size dot. What
    // happens on the way in does not differ.
    const Vec2 base = shape.radius > 0.0f
                          ? disc_half(zoom, shape.radius)
                          : disc_half(zoom, dot_px / std::max(zoom, 1e-4f));
    const Vec2 target{std::max(base.x, layout_half.x), std::max(base.y, layout_half.y)};
    return lerp(base, target, disc_morph(detail, shape, layout_half));
}

bool label_belongs_inside(float morph) { return morph > 0.5f; }

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
