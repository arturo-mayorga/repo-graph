#include "rgv/systems/GraphRenderSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/ui/Theme.h"
#include "rgv/view/SemanticZoom.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace rgv::systems {

void GraphRenderSystem::setup(ecs::World&) {
    std::string error;
    if (!renderer_.init(&error)) {
        throw std::runtime_error("renderer init failed:\n" + error);
    }
}

void GraphRenderSystem::teardown(ecs::World&) { renderer_.shutdown(); }

void GraphRenderSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto&       registry = world.registry;
    const auto& camera   = world.resource<Camera>();
    const auto& view     = world.resource<ecs::ViewSettings>();
    const auto& theme    = ui::theme();

    renderer_.begin(camera, theme.background);

    // One semantic-zoom decision for the whole frame; picking made the same one.
    const view::NodeDetail detail = view::node_detail(camera.zoom, view.graph_text_scale);

    auto half_of = [&](entt::entity e) -> Vec2 {
        const auto* ext = registry.try_get<ecs::Extent>(e);
        if (!ext) return Vec2{6.0f, 6.0f};
        const bool emphasised = registry.all_of<ecs::Selected>(e) ||
                                registry.all_of<ecs::Hovered>(e) ||
                                registry.all_of<ecs::OnExplainedPath>(e);
        const auto*           d  = registry.try_get<ecs::Disc>(e);
        const auto*           sp = registry.try_get<ecs::Spacing>(e);
        const auto*           pr = registry.try_get<ecs::Prominence>(e);
        const view::DiscShape shape{d ? d->radius : 0.0f, sp ? sp->room : 1e9f};
        return view::node_half(camera.zoom, detail, ext->half, shape,
                               view::dot_px_for(registry.all_of<ecs::Changed>(e),
                                                registry.all_of<ecs::Impacted>(e), emphasised,
                                                pr ? pr->scale : 1.0f));
    };

    auto emit_edge = [&](entt::entity ent, const ecs::Endpoints& ends, const ecs::Style& style) {
        const auto* pa = registry.try_get<ecs::Position>(ends.from);
        const auto* pb = registry.try_get<ecs::Position>(ends.to);
        if (!pa || !pb) return;

        const Vec2 dir = normalize(pb->p - pa->p);
        if (length_sq(dir) < 1e-6f) return;

        // Stop the line at the node boundary rather than the centre, so the arrowhead
        // lands on the edge of the shape at any zoom.
        auto edge_point = [](const Vec2& c, const Vec2& half, const Vec2& d) {
            const float tx = d.x != 0.0f ? half.x / std::abs(d.x) : 1e9f;
            const float ty = d.y != 0.0f ? half.y / std::abs(d.y) : 1e9f;
            return c + d * std::min(tx, ty);
        };
        const Vec2 a = edge_point(pa->p, half_of(ends.from), dir);
        const Vec2 b = edge_point(pb->p, half_of(ends.to), dir * -1.0f);

        Vec4 color = style.stroke;
        // Context recedes further as the view zooms out: at overview scale the edges
        // are what turn a readable graph into a hairball.
        if (!registry.all_of<ecs::OnExplainedPath>(ent)) {
            // The nested view keeps its boxes and names readable at overview, so its
            // edges are not faded with the zoom the way a field of dots' are. They are
            // a field, though -- hundreds at once -- so the unattended ones sit back
            // and the hovered or selected node's edges come forward at full strength.
            if (view.mode == ecs::ViewMode::Architecture) {
                // A faint constant for the field, full strength for what is attended;
                // containment is structure and stays readable.
                const bool structural = registry.get<ecs::EdgeRef>(ent).kind == EdgeKind::Contains;
                color.a *= style.emphasis >= 0.99f ? 1.0f
                           : structural            ? 0.38f
                                                   : 0.035f + 0.05f * style.emphasis;
            } else {
                color.a *= (0.35f + 0.65f * style.emphasis) * (0.45f + 0.55f * detail.t);
            }
        }
        // Width is in world units, so at overview a 1.2-unit line is a fraction of a
        // pixel and the dependencies vanish. A floor of one screen pixel keeps them.
        const float width = std::max(style.stroke_w, 1.1f / std::max(camera.zoom, 1e-4f));
        renderer_.add_edge(a, b, color, width, style.dash);
        if (view.show_arrows && detail.t > 0.15f) {
            renderer_.add_arrow(b, dir, style.stroke_w > 2.5f ? 13.0f : 9.0f, color);
        }
    };

    // Edges first, and the explained path last within that pass, so the explanation is
    // never buried under the graph it is explaining. Containment is drawn where the
    // layout is built on it: the filesystem tree and the architecture graph.
    const bool draw_containment = view.mode != ecs::ViewMode::FileGraph;
    for (auto [ent, ref, ends, style] :
         registry.view<const ecs::EdgeRef, const ecs::Endpoints, const ecs::Style>().each()) {
        if (ref.kind == EdgeKind::Contains && !draw_containment) continue;
        if (!registry.all_of<ecs::OnExplainedPath>(ent)) emit_edge(ent, ends, style);
    }
    for (auto [ent, ends, style, path] :
         registry.view<const ecs::Endpoints, const ecs::Style, const ecs::OnExplainedPath>()
             .each()) {
        emit_edge(ent, ends, style);
    }

    for (auto [ent, ref, pos, ext, style] :
         registry.view<const ecs::NodeRef, const ecs::Position, const ecs::Extent,
                       const ecs::Style>().each()) {
        const Vec2  half = half_of(ent);
        const auto* d  = registry.try_get<ecs::Disc>(ent);
        const auto* sp = registry.try_get<ecs::Spacing>(ent);
        // A circle is a box whose corners are its own radius, so the corner follows the
        // same morph the size does. Using the plain zoom curve here rounds a circle into
        // a square the moment the graph opens.
        const float shape_t = view::disc_morph(
            detail, view::DiscShape{d ? d->radius : 0.0f, sp ? sp->room : 1e9f}, ext.half);
        const float radius = 5.0f * shape_t + std::min(half.x, half.y) * (1.0f - shape_t);

        // A seed gets a halo: "the agent touched this" must be findable without reading
        // a label, which is the only cue left at dot scale.
        if (registry.all_of<ecs::Changed>(ent)) {
            const float glow = 9.0f * detail.t + 5.0f * (1.0f - detail.t);
            renderer_.add_node(pos.p, half + Vec2{glow, glow}, theme.seed_glow,
                               Vec4{0, 0, 0, 0}, 0.0f, 0.0f, radius + glow);
        }

        // A changed hub screams. Expanding rings, because a static halo is what every
        // changed node already has, and this is categorically different: most of the
        // repository depends on what just moved.
        if (const auto* hub = registry.try_get<ecs::HubSeed>(ent)) {
            const float reach = std::clamp(hub->reach_fraction, 0.25f, 1.0f);
            for (int ring = 0; ring < 3; ++ring) {
                const float phase = std::fmod(static_cast<float>(frame.time) * 0.75f +
                                                  static_cast<float>(ring) / 3.0f, 1.0f);
                const float grow =
                    (12.0f + phase * 62.0f * reach) / std::max(camera.zoom, 0.05f);
                Vec4 c = theme.changed;
                // Fade as it expands, so the rings emanate rather than blink.
                c.a = 0.55f * (1.0f - phase) * (1.0f - phase);
                renderer_.add_node(pos.p, half + Vec2{grow, grow}, Vec4{0, 0, 0, 0}, c, 2.2f,
                                   0.0f, radius + grow);
            }
        }

        Vec4 fill = style.fill;
        // A dot is mostly outline; without a lift in fill it reads as a hollow ring.
        if (!d && detail.t < 0.5f) {
            fill = mix(style.stroke, fill, 0.35f + 0.65f * detail.t * 2.0f);
        }

        // Directories get a soft halo, the way Gource blooms them -- it is what makes a
        // dense tree read as structure rather than scattered dots. It fades out as the
        // node becomes a box, where a halo reads as a second, broken rectangle rather
        // than a glow.
        // Gone entirely by the time the node is a box, not merely faint: a halo behind
        // a rectangle reads as a second, misaligned rectangle rather than a glow.
        if (d && ref.kind != NodeKind::File && shape_t < 0.5f) {
            Vec4 bloom = style.stroke;
            bloom.a    = 0.13f * (1.0f - shape_t * 2.0f);
            renderer_.add_node(pos.p, half * 1.55f, bloom, Vec4{0, 0, 0, 0}, 0.0f, 0.0f,
                               radius * 1.55f);
        }

        renderer_.add_node(pos.p, half, fill, style.stroke, style.stroke_w, style.dash, radius);
    }

    renderer_.flush();
    world.resource<ecs::SceneStats>().render_draw_calls = renderer_.stats().draw_calls;
}

} // namespace rgv::systems
