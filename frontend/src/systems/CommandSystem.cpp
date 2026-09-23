#include "rgv/systems/CommandSystem.h"

#include "rgv/ecs/Commands.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/platform/OpenPath.h"
#include "rgv/view/CameraFit.h"

#include <algorithm>
#include <cstdio>

namespace rgv::systems {

void CommandSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto& queue = world.resource<ecs::CommandQueue>();
    if (queue.empty()) return;

    auto& selection = world.resource<ecs::Selection>();
    auto& view      = world.resource<ecs::ViewSettings>();
    auto& filters   = world.resource<ecs::Filters>();
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

                } else if constexpr (std::is_same_v<T, ecs::ClearEdgeSelection>) {
                    selection.edge.clear();

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

                } else if constexpr (std::is_same_v<T, ecs::Relayout>) {
                    requests.relayout = true;
                    queue.push(ecs::FitView{});

                } else if constexpr (std::is_same_v<T, ecs::SetViewMode>) {
                    const auto mode = static_cast<ecs::ViewMode>(c.mode);
                    if (mode != view.mode) {
                        // Selection deliberately survives the switch (FR-30); only the
                        // visible node set changes.
                        view.mode  = mode;
                        view.level = ecs::default_level(mode);
                        requests.rebuild = true;

                        // Deliberately FitView rather than SceneRequests::refit.
                        // `refit` was written here and read nowhere in the tree, so the
                        // framing after a mode switch only ever happened because the
                        // toolbar pushed FitView itself. Wiring this command without
                        // choosing would have restored a path with no effect. FitView
                        // wins because it already has the one owner -- this system --
                        // and a second, flag-shaped route to the same camera is the
                        // duplicated state the architecture exists to refuse. Pushed
                        // rather than applied: SceneSyncSystem rebuilds later this
                        // frame, so fitting now would frame the scene being discarded.
                        // SceneRequests::refit is now written by nothing and should be
                        // deleted; Resources.h is not this change's to edit.
                        queue.push(ecs::FitView{});
                    }

                } else if constexpr (std::is_same_v<T, ecs::SetImpactLevel>) {
                    // Levels coexist, so this is a free switch: no re-query, no lost
                    // context, and nothing on screen has to move.
                    view.level = static_cast<Level>(c.level);

                } else if constexpr (std::is_same_v<T, ecs::SetViewToggle>) {
                    switch (c.which) {
                        case ecs::ViewToggle::LayoutRunning:    view.layout_running = c.on; break;
                        case ecs::ViewToggle::ShowLabels:       view.show_labels = c.on; break;
                        case ecs::ViewToggle::ShowArrows:       view.show_arrows = c.on; break;
                        case ecs::ViewToggle::ShowPanels:       view.show_panels = c.on; break;
                        case ecs::ViewToggle::ShowTextSettings: view.show_text_settings = c.on; break;
                    }

                } else if constexpr (std::is_same_v<T, ecs::SetTextScale>) {
                    const float v = std::clamp(c.value, config::Settings::kMinTextScale,
                                               config::Settings::kMaxTextScale);
                    if (c.which == ecs::TextScale::Ui) {
                        // Panel geometry is derived from this every frame, so there is
                        // nothing to invalidate.
                        view.ui_text_scale = v;
                    } else {
                        view.graph_text_scale = v;
                        // A node box is sized to hold its label, so the footprints go
                        // stale. Resizing in place, not rearranging: `resettle` is what
                        // pushes newly-overlapping neighbours apart.
                        requests.refresh_extents = true;
                    }

                } else if constexpr (std::is_same_v<T, ecs::SetFilterFlag>) {
                    switch (c.which) {
                        case ecs::FilterFlag::ShowUnaffected: filters.show_unaffected = c.on; break;
                        case ecs::FilterFlag::ShowStale:      filters.show_stale = c.on; break;
                        case ecs::FilterFlag::ShowHeuristic:  filters.show_heuristic = c.on; break;
                        case ecs::FilterFlag::ShowExternal:   filters.show_external = c.on; break;
                    }
                    // A revisit, never a rebuild: filters are dragged and toggled, and
                    // clearing the registry would reseed every position mid-gesture.
                    requests.revisit = true;

                } else if constexpr (std::is_same_v<T, ecs::SetImpactDepth>) {
                    filters.max_impact_depth = std::clamp(c.depth, 1, 12);
                    // Depth only decides visibility while unaffected context is hidden;
                    // otherwise every node is on screen regardless and the difference is
                    // emphasis, which needs no revisit.
                    if (!filters.show_unaffected) requests.revisit = true;

                } else if constexpr (std::is_same_v<T, ecs::SetMinRelevance>) {
                    filters.min_relevance = std::clamp(c.value, 0.0f, 1.0f);
                    requests.revisit = true;

                } else if constexpr (std::is_same_v<T, ecs::SetFilterText>) {
                    filters.text     = c.text;
                    requests.revisit = true;

                } else if constexpr (std::is_same_v<T, ecs::AddHidePattern>) {
                    // An invalid pattern is kept so the user can see and fix it, and
                    // hides nothing -- so this is not an error path.
                    ecs::add_hide_pattern(filters, c.source);
                    requests.revisit = true;

                } else if constexpr (std::is_same_v<T, ecs::RemoveHidePattern>) {
                    if (c.index >= 0 && c.index < static_cast<int>(filters.hidden.size())) {
                        filters.hidden.erase(filters.hidden.begin() + c.index);
                        requests.revisit = true;
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
                        } else if (!platform::open_with(
                                       abs,
                                       world.resource<ecs::SettingsResource>().values.open_command,
                                       &err)) {
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

                } else if constexpr (ecs::is_transport_command_v<T>) {
                    // Not ours, and deliberately dropped rather than forwarded.
                    // TransportSystem runs in Input and takes these off the queue
                    // before this system ever sees one; arriving here means it is not
                    // in the schedule, so there is nothing to move and re-queueing
                    // would just ping-pong the command forever.
                }
            },
            command);
    }
}

} // namespace rgv::systems
