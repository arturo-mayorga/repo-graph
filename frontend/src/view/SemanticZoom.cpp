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

Vec2 disc_half(float zoom, float world_radius, float min_px) {
    const float z = std::max(zoom, 1e-4f);
    const float r = std::max(world_radius, min_px / z);
    return Vec2{r, r};
}

float disc_morph(const NodeDetail& detail, const DiscShape& disc, const Vec2& layout_half) {
    // The bar for "has room" is lower than the full label width, because a name inside
    // a box is shrunk to fit it. A node only needs enough space for a legible box, not
    // for its name at full size -- otherwise anything with a long name never morphs at
    // all, however far you zoom.
    const float need = std::max(layout_half.x, 1.0f);
    const float room = std::clamp((disc.room - need * 0.30f) / (need * 0.35f), 0.0f, 1.0f);

    // Expressed in on-screen text size rather than raw zoom, so it tracks the user's
    // text-size preference instead of ignoring it.
    const float x    = std::clamp((detail.font_px - 18.0f) / 12.0f, 0.0f, 1.0f);
    const float near = x * x * (3.0f - 2.0f * x);
    return room * near;
}

Vec2 node_half(float zoom, const NodeDetail& detail, const Vec2& layout_half,
               const DiscShape* disc, float dot_px) {
    if (!disc) return render_half(zoom, detail, layout_half, dot_px);

    // A disc morphs toward its label box on the way in, the same way a dot does in the
    // other views -- but only if it has the room. Two gates, multiplied:
    //
    //   room  -- is there space between this node and its neighbours for a box? In a
    //            radial layout the answer is yes for the repository and its packages,
    //            and no for files on an orbit, which sit ~18 units apart while a
    //            filename box is ~120 wide. Those stay circles at every zoom, and are
    //            named from outside instead.
    //   zoom  -- has the user zoomed past an overview? This is the part they drive.
    //
    // The zoom gate is deliberately later than the one the box views use. That curve
    // saturates around the default fit, so reusing it turns the whole graph into
    // squashed boxes the moment it opens -- the overview has to stay a constellation
    // of circles, and boxes are what you zoom in to get.
    const Vec2 base = disc_half(zoom, disc->radius);
    const Vec2 target{std::max(base.x, layout_half.x), std::max(base.y, layout_half.y)};
    return lerp(base, target, disc_morph(detail, *disc, layout_half));
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
