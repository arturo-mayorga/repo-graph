#include "rgv/systems/CommandSystem.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/platform/OpenPath.h"
#include "rgv/view/CameraFit.h"

#include <cstdio>

namespace rgv::systems {

void CommandSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto& queue = world.resource<ecs::CommandQueue>();
    if (queue.empty()) return;

    auto& selection = world.resource<ecs::Selection>();
    auto& view      = world.resource<ecs::ViewSettings>();
    auto& requests  = world.resource<ecs::SceneRequests>();
    auto& camera    = world.resource<ecs::CameraControl>();
    auto& handle    = world.resource<ecs::SourceHandle>();
    auto& library   = world.resource<ecs::FixtureLibrary>();
    auto& settings  = world.resource<ecs::SettingsResource>();

    // Moved out first: applying a command may push another, and growing the vector
    // mid-walk would invalidate it. Anything pushed now runs on the next frame.
    const std::vector<ecs::Command> batch = std::move(queue.pending);
    queue.pending.clear();

    for (const auto& command : batch) {
        std::visit(
            [&](const auto& c) {
                using T = std::decay_t<decltype(c)>;

                if constexpr (std::is_same_v<T, ecs::SelectNode>) {
                    selection.node = c.id;
                    selection.edge.clear();
                    selection.path_index = 0;

                } else if constexpr (std::is_same_v<T, ecs::SelectEdge>) {
                    selection.edge = c.id;

                } else if constexpr (std::is_same_v<T, ecs::ClearSelection>) {
                    selection = ecs::Selection{};

                } else if constexpr (std::is_same_v<T, ecs::CyclePath>) {
                    // Wrapped against the real path count by SelectionSystem, which is
                    // the only thing that knows how many explanations exist.
                    selection.path_index += c.delta;

                } else if constexpr (std::is_same_v<T, ecs::FitView>) {
                    view::fit_camera(world, {});
                    camera.auto_fit = true;

                } else if constexpr (std::is_same_v<T, ecs::FocusNodes>) {
                    view::fit_camera(world, c.ids);
                    camera.auto_fit = false;   // the user asked for this framing

                } else if constexpr (std::is_same_v<T, ecs::SetViewMode>) {
                    const auto mode = static_cast<ecs::ViewMode>(c.mode);
                    if (mode != view.mode) {
                        // Selection deliberately survives the switch (FR-30); only the
                        // visible node set changes.
                        view.mode  = mode;
                        view.level = ecs::default_level(mode);
                        requests.rebuild = true;
                        requests.refit   = true;
                    }

                } else if constexpr (std::is_same_v<T, ecs::OpenNode>) {
                    // The desktop owns the choice of program. We only decide whether
                    // the path is one we are willing to hand it.
                    const auto& store = world.resource<GraphStore>();
                    if (const Node* n = store.node(c.id)) {
                        const std::string abs =
                            platform::openable_path(store.baseline().repo.root, n->path);
                        std::string err;
                        if (abs.empty()) {
                            std::fprintf(stderr,
                                         "rgv: not opening '%s': no such file inside %s\n",
                                         n->path.c_str(), store.baseline().repo.root.c_str());
                        } else if (!platform::open_in_default_app(abs, &err)) {
                            std::fprintf(stderr, "rgv: could not open '%s': %s\n", abs.c_str(),
                                         err.c_str());
                        }
                    }

                } else if constexpr (std::is_same_v<T, ecs::TogglePin>) {
                    const entt::entity e = world.resource<ecs::EntityIndex>().node(c.id);
                    if (e != entt::null) {
                        if (world.registry.all_of<ecs::Pinned>(e)) {
                            world.registry.remove<ecs::Pinned>(e);
                        } else {
                            world.registry.emplace<ecs::Pinned>(e);
                        }
                    }

                } else if constexpr (std::is_same_v<T, ecs::SetImpactLevel>) {
                    view.level = static_cast<Level>(c.level);

                } else if constexpr (std::is_same_v<T, ecs::SelectScenario>) {
                    if (handle.fixtures) {
                        handle.fixtures->select_scenario(static_cast<std::size_t>(c.index));
                    }

                } else if constexpr (std::is_same_v<T, ecs::SelectFixture>) {
                    if (library.load && c.index != library.current) library.load(world, c.index);

                } else if constexpr (std::is_same_v<T, ecs::ReloadFixture>) {
                    if (library.load) library.load(world, library.current);

                } else if constexpr (std::is_same_v<T, ecs::SaveSettings>) {
                    settings.values.ui_text_scale    = view.ui_text_scale;
                    settings.values.graph_text_scale = view.graph_text_scale;
                    settings.values.sanitize();
                    if (!config::save(settings.values, settings.path)) {
                        std::fprintf(stderr, "could not write %s\n", settings.path.c_str());
                    }
                }
            },
            command);
    }
}

} // namespace rgv::systems
