// Resources: the state there is exactly one of.
//
// Each of these was previously a member of a single Scene object that systems reached
// through. Making them separate resources means a system's dependencies are visible in
// its body -- it asks for Filters, or for Camera, not for "the scene".
#pragma once

#include "rgv/analysis/Reach.h"
#include "rgv/analysis/Specificity.h"
#include "rgv/contract/IGraphSource.h"
#include "rgv/config/Settings.h"
#include "rgv/render/Math.h"

#include <entt/entt.hpp>

#include <filesystem>
#include <functional>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rgv::fixture { class FixtureSource; }

namespace rgv::ecs {

class World;

// Which nodes exist on screen. Distinct from Level, which is what the backend computed
// impact at -- a user can look at files while reading package-level impact.
enum class ViewMode {
    Architecture,   // packages and their depends_on edges
    Filesystem,     // directory/file containment tree
    FileGraph,      // files and their resolved imports
};

const char* to_label(ViewMode m);

// The impact level a view reads by default. Levels coexist and the user can switch,
// but a view opens on the level whose result it can actually draw.
Level default_level(ViewMode m);

struct ViewSettings {
    ViewMode mode  = ViewMode::Architecture;
    Level    level = Level::Package;

    bool layout_running     = true;
    bool show_labels        = true;
    bool show_arrows        = true;
    bool show_text_settings = false;

    // Independent, and persisted. `ui` drives panel chrome and panel geometry; `graph`
    // drives node labels and therefore node box sizes.
    float ui_text_scale    = 1.0f;
    float graph_text_scale = 1.0f;
};

struct Filters {
    bool show_unaffected = true;   // FR-35: "only what the agent touched"
    bool show_stale      = true;
    bool show_heuristic  = true;
    bool show_external   = false;
    int  max_impact_depth = 8;

    // Hide impact that only reaches the change through a hub. Never applies to what
    // the agent changed -- see analysis/Specificity.h.
    float min_relevance = 0.0f;

    std::string text;   // substring match on name/path

    // FR-35, the other direction: hide what matches. Each pattern is a regular
    // expression tested against a node's name and path, case-insensitively; a match
    // hides the node and everything it contains, and a hidden node's edges are gone
    // rather than moved up to its package. An invalid pattern is kept so the user can
    // see and fix it, and hides nothing.
    struct HidePattern {
        std::string source;
        std::regex  re;
        bool        valid = false;
    };
    std::vector<HidePattern> hidden;
};

// Compiles and adds a pattern. Returns whether it compiled.
bool add_hide_pattern(Filters& f, std::string source);
// Whether `name` or `path` matches any valid pattern.
bool hidden_by_pattern(const Filters& f, const std::string& name, const std::string& path);

// The single source of truth for what is selected and hovered.
//
// The Selected / Hovered / OnExplainedPath components are DERIVED from this by
// SelectionSystem, and nothing else may write them. Previously the ids and the
// components were kept in step by hand, and they had already drifted: selecting a node
// from the inspector updated the ids but never the components, so the canvas showed no
// outline.
struct Selection {
    NodeId node;
    EdgeId edge;
    NodeId hovered;

    // Seconds the pointer has rested on `hovered`. Gates the hover card's fade, so
    // sweeping across a dense graph does not strobe.
    float hover_time = 0.0f;

    // `--hover` asked for this one. It holds until the pointer actually moves, which
    // is what makes the flag usable for a screenshot: a cursor resting wherever the
    // window happened to open is not input, and without this it overwrote the forced
    // hover on the first frame. Live input still wins the moment it happens.
    bool hover_pinned = false;

    // Which of several alternative dependency paths is being explained.
    int path_index = 0;
};

// Where things are on screen. Written by the window and by the panels; read by the
// camera, picking, and rendering.
struct Viewport {
    float framebuffer_w = 1.0f;
    float framebuffer_h = 1.0f;

    // The rectangle not covered by panels -- what the graph actually gets.
    Vec2 free_origin{0.0f, 0.0f};
    Vec2 free_size{0.0f, 0.0f};

    // Measured while drawing, used to size the toolbar on the next frame.
    float top_bar_height = 64.0f;

    bool contains(Vec2 p) const {
        return p.x >= free_origin.x && p.x <= free_origin.x + free_size.x &&
               p.y >= free_origin.y && p.y <= free_origin.y + free_size.y;
    }
};

// Devices, as data. The only thing permitted to read the platform is WindowSystem;
// everything downstream reads this.
struct FrameInput {
    Vec2  mouse{0.0f, 0.0f};      // framebuffer pixels
    Vec2  mouse_delta{0.0f, 0.0f};
    float wheel = 0.0f;

    bool mouse_down     = false;
    bool mouse_pressed  = false;   // edge: went down this frame
    bool mouse_released = false;
    bool double_click   = false;

    // True when a panel wants the event. Systems that act on the graph must respect
    // it or clicks land in two places at once.
    bool ui_wants_mouse    = false;
    bool ui_wants_keyboard = false;

    bool fit_pressed     = false;
    bool escape_pressed  = false;
    bool play_pressed    = false;
    bool step_pressed    = false;
    bool restart_pressed = false;
};

// What the pointer is over. Written by PickingSystem, read by NavigationSystem and the
// panels, so "what is under the cursor" is resolved once per frame rather than three
// times with three chances to disagree.
struct PointerTarget {
    entt::entity entity     = entt::null;
    bool         over_graph = false;   // inside the free rect and not captured by a panel
};

// A drag in progress. Navigation records it; layout applies it, because moving a node
// is a layout question: a directory has to take its files with it, and its neighbours
// have to get out of the way.
struct DragState {
    entt::entity node   = entt::null;
    Vec2         delta{0.0f, 0.0f};
    bool         active = false;
};

// While true the camera keeps framing the graph. Any manual pan, zoom, or drag clears
// it: once the user has placed the view, layout stops moving it.
struct CameraControl {
    bool auto_fit = true;
};

// Derived once per change and read by several systems: the IDF index over the current
// graph, and any change to a node most of the repository depends on.
struct DerivedState {
    analysis::SpecificityIndex      specificity;
    analysis::ReachIndex            reach;
    std::vector<analysis::HubAlert> hub_alerts;
};

// id -> entity, maintained by SceneSyncSystem. A resource rather than a private map
// because selection, picking, and the panels all need to resolve ids.
struct EntityIndex {
    std::unordered_map<NodeId, entt::entity> nodes;
    std::unordered_map<EdgeId, entt::entity> edges;
    // Edges that are not drawn because other drawn edges explain them: a package-level
    // `depends_on` whose modules have an edge of their own on screen. Marking a path
    // through the hidden edge marks these instead, so an explanation at package level
    // lights the module edges that constitute it.
    std::unordered_map<EdgeId, std::vector<EdgeId>> aliases;

    entt::entity node(const NodeId& id) const {
        auto it = nodes.find(id);
        return it == nodes.end() ? entt::null : it->second;
    }
    entt::entity edge(const EdgeId& id) const {
        auto it = edges.find(id);
        return it == edges.end() ? entt::null : it->second;
    }
};

struct SceneStats {
    int nodes = 0, edges = 0;
    int changed = 0, impacted = 0, stale = 0;
    int muted  = 0;  // impacted, but below the relevance threshold
    int hidden = 0;  // hubs removed from the view entirely by that threshold

    int render_draw_calls = 0;
    float layout_energy   = 0.0f;
    bool  layout_settled  = false;
};

struct FrameTiming {
    float fps      = 60.0f;   // smoothed
    float frame_ms = 0.0f;
};

// The platform window. Owned by WindowSystem; the only handle to the OS in the world.
struct WindowHandle {
    void* window       = nullptr;   // GLFWwindow*, opaque so core need not know GLFW
    bool  should_close = false;
};

// The attached data source. Swapping what lives here -- a fixture player today, a
// filesystem watcher later -- is the whole extension point.
struct SourceHandle {
    IGraphSource* source = nullptr;
    // Non-null exactly when the source is replayable. The only capability check in the
    // codebase, and the reason a scrubber appears.
    fixture::FixtureSource* fixtures = nullptr;
};

// The fixture sets discovered on disk, and the ability to swap between them.
//
// `load` is installed by whoever constructed the source, because only it knows how to
// build one. CommandSystem calls it; it does not need to know what a fixture is.
struct FixtureLibrary {
    std::vector<std::string> dirs;
    std::vector<std::string> names;
    int                      current = 0;

    std::function<bool(World&, int)> load;
};

// User preferences and where they came from.
struct SettingsResource {
    config::Settings      values;
    std::filesystem::path path;
};

// Set by any system that needs the scene rebuilt or refitted before the next frame.
// Distinct from CommandQueue: these are idempotent flags, not an ordered log.
struct SceneRequests {
    // Teardown. The visible node set is a function of the view MODE, so switching mode
    // is not a delta -- nothing on screen survives it. Everything else should not be
    // doing this: clearing the registry throws away every position, and doing it while
    // a slider is being dragged is what jitter looks like.
    bool rebuild = false;

    // The filters moved. Re-evaluate what is visible and add or drop the difference,
    // keeping every entity that survives -- and with it, where it already is.
    bool revisit = false;

    bool refresh_extents = false;  // text scale moved; node boxes need resizing

    // Recompute the whole layout from scratch. Every node moves, so this is for when
    // the user asks for it and for a mode change -- never for nodes coming and going.
    // Newcomers are placed where they belong and left to the relaxation instead.
    bool relayout = false;

    // Something changed the size of what is already placed. Nothing needs to move to a
    // new home, but neighbours may now overlap, so the relaxation is woken to push them
    // apart -- which is a nudge, not a rearrangement.
    bool resettle = false;

    bool refit = false;
};

} // namespace rgv::ecs
