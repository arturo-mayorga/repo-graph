#include "rgv/systems/NavigationSystem.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/view/CameraFit.h"

#include <algorithm>
#include <cmath>

namespace rgv::systems {

void NavigationSystem::run(ecs::World& world, const ecs::FrameContext&) {
    const auto& input   = world.resource<ecs::FrameInput>();
    const auto& target  = world.resource<ecs::PointerTarget>();
    const auto& stats   = world.resource<ecs::SceneStats>();
    auto&       camera  = world.resource<Camera>();
    auto&       control = world.resource<ecs::CameraControl>();
    auto&       queue   = world.resource<ecs::CommandQueue>();
    auto&       registry = world.registry;

    // -- zoom about the cursor, so the thing under the pointer stays under it
    if (target.over_graph && input.wheel != 0.0f) {
        const Vec2  before = camera.screen_to_world(input.mouse);
        const float factor = std::pow(1.14f, input.wheel);
        camera.zoom        = std::clamp(camera.zoom * factor, 0.02f, 6.0f);
        const Vec2 after   = camera.screen_to_world(input.mouse);
        camera.center += before - after;
        control.auto_fit = false;
    }

    // -- gesture start: on a node it is a drag, on empty space it is a pan
    if (target.over_graph && input.mouse_pressed) {
        if (target.entity != entt::null) dragging_ = target.entity;
        else panning_ = true;
    }
    if (!input.mouse_down) {
        dragging_ = entt::null;
        panning_  = false;
    }

    if (input.mouse_down && length_sq(input.mouse_delta) > 0.0f) {
        if (dragging_ != entt::null && registry.valid(dragging_)) {
            if (auto* pos = registry.try_get<ecs::Position>(dragging_)) {
                pos->p += input.mouse_delta / camera.zoom;
                // Dragging pins: the user has said where this one goes.
                registry.emplace_or_replace<ecs::Pinned>(dragging_);
                if (auto* t = registry.try_get<ecs::LayoutTarget>(dragging_)) t->p = pos->p;
                control.auto_fit = false;
            }
        } else if (panning_) {
            camera.center -= input.mouse_delta / camera.zoom;
            control.auto_fit = false;
        }
    }

    if (target.over_graph && input.double_click && target.entity != entt::null) {
        if (registry.all_of<ecs::Pinned>(target.entity)) registry.remove<ecs::Pinned>(target.entity);
        else registry.emplace<ecs::Pinned>(target.entity);
    }

    if (input.fit_pressed) queue.push(ecs::FitView{});
    if (input.escape_pressed) queue.push(ecs::ClearSelection{});

    // Keep the graph framed until it stops moving. Fitting once is not enough: layout
    // eases toward its targets, and the graph would drift off screen.
    if (control.auto_fit && !stats.layout_settled) view::fit_camera(world, {});
}

} // namespace rgv::systems
