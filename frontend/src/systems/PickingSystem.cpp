#include "rgv/systems/PickingSystem.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/view/SemanticZoom.h"

#include <cmath>

namespace rgv::systems {
namespace {

entt::entity pick(ecs::World& world, Vec2 screen) {
    auto&       registry = world.registry;
    const auto& camera   = world.resource<Camera>();
    const auto& view     = world.resource<ecs::ViewSettings>();

    const Vec2            w      = camera.screen_to_world(screen);
    const view::NodeDetail detail = view::node_detail(camera.zoom, view.graph_text_scale);
    const float           zoom   = std::max(camera.zoom, 1e-4f);

    entt::entity best      = entt::null;
    float        best_area = 0.0f;
    float        best_dist = 0.0f;

    // NodeRef named explicitly: edges lacking Position is an accident of the current
    // archetypes, not something to build a query on.
    for (auto [ent, ref, pos, ext] :
         registry.view<const ecs::NodeRef, const ecs::Position, const ecs::Extent>().each()) {
        const bool  changed  = registry.all_of<ecs::Changed>(ent);
        const bool  impacted = registry.all_of<ecs::Impacted>(ent);
        const auto* d        = registry.try_get<ecs::Disc>(ent);
        const view::DiscShape shape{d ? d->radius : 0.0f, d ? d->room : 1e9f};
        Vec2        half     = view::node_half(camera.zoom, detail, ext.half,
                                               d ? &shape : nullptr,
                                               view::dot_px_for(changed, impacted, false));

        // A dot must stay clickable even when it is a few pixels across, so the hit
        // area has a screen-space floor. Without it, overview zoom becomes a test of
        // mouse precision.
        const float min_world = 7.0f / zoom;
        half.x = std::max(half.x, min_world);
        half.y = std::max(half.y, min_world);

        if (std::abs(w.x - pos.p.x) > half.x || std::abs(w.y - pos.p.y) > half.y) continue;

        // Smallest hit wins, so a small node inside a big cluster stays clickable.
        // Ties break on distance to centre, because once every node is collapsed to the
        // same minimum size their areas are identical and their hit areas overlap --
        // without this, which node a click selects is arbitrary.
        const float area = half.x * half.y;
        const float dist = length_sq(w - pos.p);
        const float eps  = area * 1e-3f;
        if (best == entt::null || area < best_area - eps ||
            (std::abs(area - best_area) <= eps && dist < best_dist)) {
            best      = ent;
            best_area = area;
            best_dist = dist;
        }
    }
    return best;
}

} // namespace

void PickingSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    const auto& input    = world.resource<ecs::FrameInput>();
    const auto& viewport = world.resource<ecs::Viewport>();
    auto&       target   = world.resource<ecs::PointerTarget>();
    auto&       selection = world.resource<ecs::Selection>();

    target.over_graph = !input.ui_wants_mouse && viewport.contains(input.mouse);
    if (!target.over_graph) {
        // The hover state is left as it is rather than cleared: a card stays up while
        // the user reaches for a panel control, and a forced hover survives for a
        // screenshot.
        target.entity = entt::null;
        return;
    }

    target.entity = pick(world, input.mouse);

    NodeId id;
    if (target.entity != entt::null) {
        if (const auto* ref = world.registry.try_get<ecs::NodeRef>(target.entity)) id = ref->id;
    }

    // The dwell timer is what gates the hover card: it appears once the pointer has
    // settled on something, not while sweeping past it.
    if (!id.empty() && id == selection.hovered) {
        selection.hover_time += frame.dt;
    } else {
        selection.hovered   = id;
        selection.hover_time = 0.0f;
    }
    if (input.mouse_down) selection.hover_time = 0.0f;   // dragging is not dwelling

    if (input.mouse_pressed && !id.empty()) {
        world.resource<ecs::CommandQueue>().push(ecs::SelectNode{id});
    }
}

} // namespace rgv::systems
