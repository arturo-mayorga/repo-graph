// rgv -- Live Repository Impact Graph, native frontend.
//
// This file does three things: describe the world, attach the systems, and turn the
// crank. All behaviour lives in systems, and the schedule below IS the frame -- reading
// it top to bottom tells you exactly what happens and in what order. `rgv --schedule`
// prints the same list.
//
// Attaching a different data source means constructing a different IGraphSource.
// Adding a capability means adding a system. Neither requires touching this file.

#include "FixtureBootstrap.h"
#include "Options.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Resources.h"
#include "rgv/ecs/System.h"
#include "rgv/ecs/World.h"
#include "rgv/model/GraphStore.h"

#include "rgv/systems/CommandSystem.h"
#include "rgv/systems/GraphRenderSystem.h"
#include "rgv/systems/ImpactStateSystem.h"
#include "rgv/systems/LayoutSystem.h"
#include "rgv/systems/NavigationSystem.h"
#include "rgv/systems/PickingSystem.h"
#include "rgv/systems/SceneSyncSystem.h"
#include "rgv/systems/SelectionSystem.h"
#include "rgv/systems/SourceSystem.h"
#include "rgv/systems/SpecificitySystem.h"
#include "rgv/systems/StyleSystem.h"
#include "rgv/systems/TransportSystem.h"
#include "rgv/systems/UiSystem.h"
#include "rgv/systems/WindowSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>

#ifndef RGV_FIXTURE_DIR
#define RGV_FIXTURE_DIR "fixtures"
#endif

using namespace rgv;

namespace {

// Everything there is exactly one of. Installed before any system runs, so a system
// asking for a resource it was not given fails at startup rather than silently.
void install_resources(ecs::World& world, const app::Options& options) {
    world.add_resource<GraphStore>();
    world.add_resource<ecs::ViewSettings>();
    world.add_resource<ecs::Filters>();
    world.add_resource<ecs::Selection>();
    world.add_resource<ecs::Viewport>();
    world.add_resource<ecs::FrameInput>();
    world.add_resource<ecs::PointerTarget>();
    world.add_resource<ecs::CameraControl>();
    world.add_resource<ecs::DerivedState>();
    world.add_resource<ecs::EntityIndex>();
    world.add_resource<ecs::SceneStats>();
    world.add_resource<ecs::FrameTiming>();
    world.add_resource<ecs::SceneRequests>();
    world.add_resource<ecs::CommandQueue>();
    world.add_resource<ecs::SourceHandle>();
    world.add_resource<ecs::WindowHandle>();
    world.add_resource<Camera>();

    // Preferences load before anything is drawn, so the first frame is already at the
    // user's text size rather than snapping to it a frame later.
    auto& settings  = world.add_resource<ecs::SettingsResource>();
    settings.path   = config::settings_path();
    settings.values = config::load(settings.path);

    auto& view            = world.resource<ecs::ViewSettings>();
    view.ui_text_scale    = settings.values.ui_text_scale;
    view.graph_text_scale = settings.values.graph_text_scale;

    if (options.relevance >= 0.0f) {
        world.resource<ecs::Filters>().min_relevance = std::clamp(options.relevance, 0.0f, 1.0f);
    }
}

// The frame, in order. Six phases; insertion order within each.
void install_systems(ecs::Schedule& schedule, const app::Options& options) {
    using namespace rgv::systems;
    using ecs::Phase;

    schedule
        // Devices and the OS become resources; nothing else may read the platform.
        .add(Phase::Input, std::make_unique<WindowSystem>(
                               WindowConfig{1680, 1000, "rgv - live repository impact graph",
                                            options.fullscreen, true}))
        .add(Phase::Input, std::make_unique<PickingSystem>())
        .add(Phase::Input, std::make_unique<NavigationSystem>())
        .add(Phase::Input, std::make_unique<TransportSystem>())

        // The outside world comes in. Swap what is attached here -- a fixture player
        // today, a filesystem watcher later -- and nothing downstream changes.
        .add(Phase::Ingest, std::make_unique<SourceSystem>())

        // Reconcile it into entities.
        .add(Phase::Sync, std::make_unique<CommandSystem>())
        .add(Phase::Sync, std::make_unique<SpecificitySystem>())
        .add(Phase::Sync, std::make_unique<SceneSyncSystem>())

        // Derive everything else.
        .add(Phase::Simulate, std::make_unique<ImpactStateSystem>())
        .add(Phase::Simulate, std::make_unique<SelectionSystem>())
        .add(Phase::Simulate, std::make_unique<LayoutSystem>())
        .add(Phase::Simulate, std::make_unique<StyleSystem>())

        // Draw. Panels first: they decide how much room the graph gets.
        .add(Phase::Render, std::make_unique<UiSystem>())
        .add(Phase::Render, std::make_unique<GraphRenderSystem>())
        .add(Phase::Render, std::make_unique<OverlaySystem>())

        .add(Phase::Present, std::make_unique<PresentSystem>());
}

// Startup state applied once the graph exists and has settled: the nodes it names have
// to be there first, which is what makes --select and --hover reproducible.
void apply_startup_state(ecs::World& world, app::Options& options) {
    if (!world.resource<ecs::SceneStats>().layout_settled) return;

    const auto& index  = world.resource<ecs::EntityIndex>();
    auto&       handle = world.resource<ecs::SourceHandle>();

    if (options.at >= 0.0) {
        if (Timeline* t = handle.source ? handle.source->timeline() : nullptr) {
            t->pause();
            t->seek_ms(options.at);
        }
        options.at = -1.0;
    }
    if (!options.select.empty() && index.node(options.select) != entt::null) {
        world.resource<ecs::CommandQueue>().push(ecs::SelectNode{options.select});
        options.select.clear();
    }
    if (!options.hover.empty() && index.node(options.hover) != entt::null) {
        auto& selection      = world.resource<ecs::Selection>();
        selection.hovered    = options.hover;
        selection.hover_time = 10.0f;   // past the dwell delay, fully faded in
        options.hover.clear();
    }
    if (options.text_settings) {
        world.resource<ecs::ViewSettings>().show_text_settings = true;
        options.text_settings = false;
    }
}

} // namespace

int main(int argc, char** argv) {
    app::Options options;
    if (!app::parse_options(argc, argv, RGV_FIXTURE_DIR, options)) return 0;

    ecs::World    world;
    ecs::Schedule schedule;
    install_resources(world, options);
    install_systems(schedule, options);

    if (options.list_schedule) {
        for (const auto& line : schedule.listing()) std::cout << line << "\n";
        return 0;
    }

    if (!app::attach_fixture_source(world, options.root, options.fixture, options.scenario)) {
        return 1;
    }

    try {
        schedule.setup(world);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "startup failed: %s\n", ex.what());
        return 1;
    }

    const auto        start = std::chrono::steady_clock::now();
    auto              last  = start;
    ecs::FrameContext frame;

    while (!world.resource<ecs::WindowHandle>().should_close) {
        const auto now = std::chrono::steady_clock::now();
        frame.dt   = std::min(0.1f, std::chrono::duration<float>(now - last).count());
        frame.time = std::chrono::duration<double>(now - start).count();
        ++frame.index;
        last = now;

        schedule.run(world, frame);
        apply_startup_state(world, options);
    }

    schedule.teardown(world);
    return 0;
}
