// Shared test scaffolding: a world with resources installed and a schedule of the
// headless systems, so tests exercise the real pipeline rather than a stand-in.
#pragma once

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/ecs/System.h"
#include "rgv/ecs/World.h"
#include "rgv/model/GraphStore.h"

#include "rgv/systems/CommandSystem.h"
#include "rgv/systems/ImpactStateSystem.h"
#include "rgv/systems/NavigationSystem.h"
#include "rgv/systems/PickingSystem.h"
#include "rgv/systems/LabelSystem.h"
#include "rgv/systems/LayoutSystem.h"
#include "rgv/systems/SceneSyncSystem.h"
#include "rgv/systems/SelectionSystem.h"
#include "rgv/systems/SpecificitySystem.h"
#include "rgv/systems/CycleSystem.h"
#include "rgv/systems/FocusSystem.h"
#include "rgv/systems/ShapeSystem.h"
#include "rgv/systems/StyleSystem.h"

#include <memory>

namespace rgvtest {

// Everything except the platform, the GPU, and the source. Ticking this is exactly
// what the application does between reading input and drawing.
struct Harness {
    rgv::ecs::World    world;
    rgv::ecs::Schedule schedule;

    Harness() {
        using namespace rgv;
        using namespace rgv::ecs;

        world.add_resource<GraphStore>();
        world.add_resource<ViewSettings>();
        world.add_resource<Filters>();
        world.add_resource<Selection>();
        world.add_resource<Viewport>();
        world.add_resource<FrameInput>();
        world.add_resource<PointerTarget>();
        world.add_resource<CameraControl>();
        world.add_resource<DragState>();
        world.add_resource<DerivedState>();
        world.add_resource<CycleReport>();
        world.add_resource<EntityIndex>();
        world.add_resource<SceneStats>();
        world.add_resource<FrameTiming>();
        world.add_resource<SceneRequests>();
        world.add_resource<CommandQueue>();
        world.add_resource<SourceHandle>();
        world.add_resource<WindowHandle>();
        world.add_resource<FixtureLibrary>();
        world.add_resource<SettingsResource>();
        world.add_resource<Camera>();

        auto& viewport         = world.resource<Viewport>();
        viewport.framebuffer_w = 1200.0f;
        viewport.framebuffer_h = 800.0f;
        viewport.free_size     = rgv::Vec2{1200.0f, 800.0f};

        schedule.add(Phase::Input, std::make_unique<systems::PickingSystem>())
            .add(Phase::Input, std::make_unique<systems::NavigationSystem>())
            .add(Phase::Sync, std::make_unique<systems::CommandSystem>())
            .add(Phase::Sync, std::make_unique<systems::SpecificitySystem>())
            .add(Phase::Sync, std::make_unique<systems::SceneSyncSystem>())
            .add(Phase::Simulate, std::make_unique<systems::ImpactStateSystem>())
            .add(Phase::Simulate, std::make_unique<systems::SelectionSystem>())
            .add(Phase::Simulate, std::make_unique<systems::FocusSystem>())
            .add(Phase::Simulate, std::make_unique<systems::LayoutSystem>())
            .add(Phase::Simulate, std::make_unique<systems::ShapeSystem>())
            .add(Phase::Simulate, std::make_unique<systems::CycleSystem>())
            .add(Phase::Simulate, std::make_unique<systems::StyleSystem>())
            .add(Phase::Simulate, std::make_unique<systems::LabelSystem>());
        schedule.setup(world);
    }

    rgv::GraphStore&        store() { return world.resource<rgv::GraphStore>(); }
    rgv::ecs::ViewSettings& view() { return world.resource<rgv::ecs::ViewSettings>(); }
    rgv::ecs::Filters&      filters() { return world.resource<rgv::ecs::Filters>(); }
    rgv::ecs::Selection&    selection() { return world.resource<rgv::ecs::Selection>(); }
    rgv::ecs::SceneStats&   stats() { return world.resource<rgv::ecs::SceneStats>(); }
    rgv::ecs::EntityIndex&  index() { return world.resource<rgv::ecs::EntityIndex>(); }
    rgv::ecs::CommandQueue& commands() { return world.resource<rgv::ecs::CommandQueue>(); }
    entt::registry&         registry() { return world.registry; }

    entt::entity node(const rgv::NodeId& id) { return index().node(id); }
    entt::entity edge(const rgv::EdgeId& id) { return index().edge(id); }

    void tick(float dt = 1.0f / 60.0f, int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            rgv::ecs::FrameContext frame;
            frame.dt    = dt;
            frame.index = ++frame_index_;
            schedule.run(world, frame);
        }
    }

    // Runs until layout stops moving, so a test can assert on settled positions.
    void settle(int max_frames = 400) {
        for (int i = 0; i < max_frames; ++i) {
            tick();
            if (stats().layout_settled) return;
        }
    }

    void request_rebuild() { world.resource<rgv::ecs::SceneRequests>().rebuild = true; }

    rgv::ecs::FrameInput& input() { return world.resource<rgv::ecs::FrameInput>(); }
    rgv::Camera&          camera() { return world.resource<rgv::Camera>(); }

    // Puts the pointer somewhere and runs a frame, so picking and navigation are
    // exercised through the same systems the application uses.
    void point_at(rgv::Vec2 screen, bool press = false) {
        auto& in         = input();
        in.mouse_delta   = screen - in.mouse;
        in.mouse         = screen;
        in.mouse_pressed = press;
        in.mouse_down    = press;
        tick();
        in.mouse_pressed = false;
    }

    rgv::ecs::PointerTarget& pointer() { return world.resource<rgv::ecs::PointerTarget>(); }

    // Drives a drag through the real input path, so NavigationSystem and LayoutSystem
    // are exercised rather than DragState being written by hand.
    void begin_drag(rgv::Vec2 screen) {
        point_at(screen);
        point_at(screen, /*press=*/true);
    }
    void drag_by(rgv::Vec2 delta) {
        auto& in       = input();
        in.mouse_down    = true;
        in.mouse_pressed = false;
        in.mouse_delta   = delta;
        in.mouse += delta;
        tick();
    }
    // Button still down, pointer stationary. The node must stay under the cursor.
    void hold_drag(int frames) {
        auto& in       = input();
        in.mouse_down    = true;
        in.mouse_pressed = false;
        in.mouse_delta   = rgv::Vec2{0.0f, 0.0f};
        tick(1.0f / 60.0f, frames);
    }
    void end_drag() {
        auto& in       = input();
        in.mouse_down  = false;
        in.mouse_delta = rgv::Vec2{0.0f, 0.0f};
        tick();
    }

private:
    std::uint64_t frame_index_ = 0;
};

} // namespace rgvtest
