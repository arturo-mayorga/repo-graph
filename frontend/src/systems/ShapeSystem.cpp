#include "rgv/systems/ShapeSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"

namespace rgv::systems {

void ShapeSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&      registry = world.registry;
    const auto mode     = world.resource<ecs::ViewSettings>().mode;

    // The only `mode -> shape` decision in the codebase. Keep it that way.
    const bool circles = mode == ecs::ViewMode::Filesystem;

    for (auto [e, ref] : registry.view<const ecs::NodeRef>().each()) {
        ecs::NodeShape shape;
        if (circles) {
            // A view that draws circles still needs the packing to have produced one.
            // A node arriving between a layout and this pass has no disc yet, and a
            // box that will be a circle next frame is better than a circle of radius
            // zero, which reads as a missing node.
            if (const auto* d = registry.try_get<ecs::Disc>(e)) {
                shape.form    = ecs::NodeShape::Form::Disc;
                shape.radius  = d->radius;
                shape.halo    = d->halo;
                shape.outward = d->outward;
            }
        }
        registry.emplace_or_replace<ecs::NodeShape>(e, shape);
    }
}

} // namespace rgv::systems
