#include "rgv/view/CameraFit.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"

#include <algorithm>

namespace rgv::view {

void fit_camera(ecs::World& world, const std::vector<NodeId>& ids, float padding) {
    auto&       registry = world.registry;
    const auto& index    = world.resource<ecs::EntityIndex>();
    const auto& viewport = world.resource<ecs::Viewport>();
    auto&       camera   = world.resource<Camera>();

    bool any = false;
    Vec2 lo{0, 0}, hi{0, 0};
    auto add_half = [&](const ecs::Position& p, const Vec2& half) {
        const Vec2 a = p.p - half, b = p.p + half;
        if (!any) { lo = a; hi = b; any = true; return; }
        lo.x = std::min(lo.x, a.x); lo.y = std::min(lo.y, a.y);
        hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y);
    };
    // A container's footprint is its hull, not the box its name would need.
    auto add = [&](const ecs::Position& p, const ecs::Extent& e) { add_half(p, e.half); };
    (void)add;

    if (ids.empty()) {
        // NodeRef named explicitly: edges lacking Position is an accident of the
        // current archetypes, not something to build a query on.
        for (auto [e, ref, p, x] :
             registry.view<const ecs::NodeRef, const ecs::Position, const ecs::Extent>().each()) {
            const auto* hull = registry.try_get<ecs::Hull>(e);
            add_half(p, hull ? hull->half : x.half);
        }
    } else {
        for (const auto& id : ids) {
            const entt::entity e = index.node(id);
            if (e == entt::null) continue;
            const auto* p = registry.try_get<ecs::Position>(e);
            const auto* x = registry.try_get<ecs::Extent>(e);
            const auto* hull = registry.try_get<ecs::Hull>(e);
            if (p && x) add_half(*p, hull ? hull->half : x->half);
        }
    }
    if (!any) return;

    camera.center = (lo + hi) * 0.5f;

    const Vec2 fit = viewport.free_size.x > 1.0f
                         ? viewport.free_size
                         : Vec2{viewport.framebuffer_w, viewport.framebuffer_h};
    const float w = std::max(1.0f, hi.x - lo.x) + padding * 2.0f;
    const float h = std::max(1.0f, hi.y - lo.y) + padding * 2.0f;
    camera.zoom   = std::clamp(std::min(fit.x / w, fit.y / h), 0.02f, 3.0f);
}

} // namespace rgv::view
