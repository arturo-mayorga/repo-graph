#include "rgv/systems/NavigationSystem.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/view/CameraFit.h"

#include <algorithm>
#include <cmath>

namespace rgv::systems {

void NavigationSystem::run(ecs::World& world, const ecs::FrameContext&) {
    const auto& viewport = world.resource<ecs::Viewport>();
    const auto& input    = world.resource<ecs::FrameInput>();
    const auto& target  = world.resource<ecs::PointerTarget>();
    const auto& stats   = world.resource<ecs::SceneStats>();
    auto&       camera  = world.resource<Camera>();
    auto&       control = world.resource<ecs::CameraControl>();
    auto&       queue   = world.resource<ecs::CommandQueue>();
    auto&       registry = world.registry;

    // The camera belongs to this system, not to the renderer. It is anchored on the
    // area the panels leave rather than the whole framebuffer, so fitting frames the
    // graph in the space the user can actually see -- and so anything that reasons in
    // screen coordinates, picking included, agrees with what is drawn.
    camera.vw     = viewport.framebuffer_w;
    camera.vh     = viewport.framebuffer_h;
    camera.anchor = viewport.free_size.x > 1.0f
                        ? viewport.free_origin + viewport.free_size * 0.5f
                        : Vec2{-1.0f, -1.0f};

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

    // Recorded, not applied. Moving a node is a layout question -- a directory takes
    // its files with it -- so LayoutSystem owns the actual movement.
    auto& drag  = world.resource<ecs::DragState>();
    drag.active = false;
    drag.delta  = Vec2{0.0f, 0.0f};

    // `active` means a drag is in progress, NOT that the pointer moved this frame.
    //
    // Gating it on movement meant that pausing mid-drag looked like a release: the node
    // stopped being held, its springs took over, and it crawled out from under a cursor
    // the user had not let go of. The delta is allowed to be zero.
    if (input.mouse_down) {
        if (dragging_ != entt::null && registry.valid(dragging_)) {
            drag.node   = dragging_;
            drag.delta  = input.mouse_delta / camera.zoom;
            drag.active = true;
            // Deliberately does NOT pin. A drag that pins turns every node the user has
            // ever touched into a fixed point, and after a few of those the graph stops
            // reacting to anything and becomes a static picture. Releasing hands the
            // node back to the relaxation, which settles it somewhere consistent with
            // its neighbours. Pinning is still available, explicitly, on double click.
            control.auto_fit = false;
        } else if (panning_ && length_sq(input.mouse_delta) > 0.0f) {
            camera.center -= input.mouse_delta / camera.zoom;
            control.auto_fit = false;
        }
    }

    if (target.over_graph && input.double_click && target.entity != entt::null) {
        // In the architecture view a package opens instead of pinning: what it holds
        // is the next level of the diagram, and that is what a double click is for
        // there. Everything else still pins.
        const auto* ref  = registry.try_get<ecs::NodeRef>(target.entity);
        const auto& view = world.resource<ecs::ViewSettings>();
        if (view.mode == ecs::ViewMode::Architecture && ref && ref->kind == NodeKind::Package) {
            world.resource<ecs::CommandQueue>().push(ecs::ToggleExpand{ref->id});
        } else if (registry.all_of<ecs::Pinned>(target.entity)) {
            registry.remove<ecs::Pinned>(target.entity);
        } else {
            registry.emplace<ecs::Pinned>(target.entity);
        }
    }

    if (input.fit_pressed) queue.push(ecs::FitView{});
    if (input.escape_pressed) queue.push(ecs::ClearSelection{});

    // Keep the graph framed until it stops moving. Fitting once is not enough: layout
    // eases toward its targets, and the graph would drift off screen.
    if (control.auto_fit && !stats.layout_settled) view::fit_camera(world, {});
}

} // namespace rgv::systems
