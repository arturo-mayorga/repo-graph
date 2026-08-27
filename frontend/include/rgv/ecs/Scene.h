// Scene owns the ECS registry and the view state, and rebuilds itself from GraphStore
// deltas. It is the boundary between "what the backend said" and "what is on screen".
#pragma once

#include "rgv/analysis/Specificity.h"
#include "rgv/ecs/Components.h"
#include "rgv/model/GraphStore.h"
#include "rgv/render/Math.h"

#include <entt/entt.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace rgv::ecs {

// Which nodes exist on screen. Distinct from Level, which is what the backend
// computed impact at -- the user can look at files while reading package impact.
// ImGui's default font. Kept as a constant here because node geometry is derived from
// text metrics, and the ECS layer must not depend on the UI toolkit to know them.
inline constexpr float kBaseFontPx      = 13.0f;
inline constexpr float kCharAdvanceRatio = 0.55f;   // advance / font size for ProggyClean

enum class ViewMode {
    Architecture,   // packages and their depends_on edges
    Filesystem,     // directory/file containment tree
    FileGraph,      // files and their resolved imports
};

const char* to_label(ViewMode m);

struct Filters {
    bool show_unaffected      = true;   // FR-35: "only what the agent touched"
    bool show_stale           = true;
    bool show_heuristic       = true;
    bool show_external        = false;
    int  max_impact_depth     = 8;

    // Hide impacted nodes whose explanation runs through a hub. 0 shows everything;
    // turning it up strips out the "and it depends on logger, like everything else"
    // results. Never applies to changed nodes -- see analysis/Specificity.h.
    float min_relevance = 0.0f;
    std::string text;                   // substring match on name/path
};

struct ViewState {
    ViewMode mode  = ViewMode::Architecture;
    Level    level = Level::Package;     // which impact result to read

    NodeId selected_node;
    EdgeId selected_edge;
    NodeId hovered_node;

    // Which of the selected node's alternative dependency paths is being explained.
    int path_index = 0;

    Filters filters;
    Camera  camera;

    // Two independent, persisted text sizes. `ui` drives panel chrome and panel
    // geometry; `graph` drives node labels and therefore node box sizes. See
    // rgv/config/Settings.h for why they are separate.
    float ui_text_scale    = 1.0f;
    float graph_text_scale = 1.0f;

    // Seconds the pointer has rested on `hovered_node`. Drives the hover card's delay
    // and fade, so sweeping the mouse across the graph does not strobe cards.
    float hover_time = 0.0f;

    // Centre of the area not covered by panels, and its size. Maintained by the app
    // each frame so fitting and picking agree with what is actually visible.
    Vec2 free_size{0.0f, 0.0f};

    // While true the camera keeps framing the graph. Any manual pan, zoom, or drag
    // clears it -- once the user has placed the view, layout stops moving it.
    bool auto_fit       = true;

    bool layout_running     = true;
    bool show_labels        = true;
    bool show_arrows        = true;
    bool show_text_settings = false;
};

// Semantic zoom. Below the point where a label is readable there is no value in
// drawing a labelled box: the node becomes a small constant-size dot, and the hover
// card takes over the job of saying what it is.
struct NodeDetail {
    float t       = 1.0f;   // 0 = compact dot, 1 = full labelled box
    float font_px = kBaseFontPx;
    bool  labels  = true;
};

NodeDetail node_detail(const ViewState& view);

// Rendered half-extent for a node. `layout_half` is the footprint layout reserved --
// which never changes with zoom, so panning and zooming cannot reflow the graph --
// while the drawn size collapses toward a dot of `dot_px` screen pixels.
//
// Picking calls this too. If it did not, clicks would land on boxes that are no
// longer being drawn.
Vec2 render_half(const ViewState& view, const NodeDetail& detail, const Vec2& layout_half,
                 float dot_px);

// Screen-space dot size for a node, larger for things the user is meant to notice.
// At overview zoom the blast radius should read as a constellation, not a uniform mesh.
float dot_px_for(bool changed, bool impacted, bool emphasised);

class Scene {
public:
    entt::registry registry;
    ViewState      view;

    // Full rebuild. Called on baseline reset and whenever the view mode changes,
    // because the visible node set is a function of the mode.
    void rebuild(const GraphStore& store);

    // Incremental update from the store's dirty set. Positions of untouched nodes are
    // preserved, which is what stops a file save from reshuffling the screen.
    void sync(const GraphStore& store);

    entt::entity find_node(const NodeId& id) const;
    entt::entity find_edge(const EdgeId& id) const;

    // Node under a screen-space point, or entt::null. Topmost (smallest) wins so a
    // small node inside a cluster stays clickable.
    entt::entity pick(Vec2 screen) const;

    // Frames the given nodes in the viewport. Empty => frame everything visible.
    void focus_on(const std::vector<NodeId>& ids, float padding = 80.0f);

    // Recomputes every node's box from its label and the current text scale. Cheaper
    // than a rebuild and, unlike one, it keeps the positions the user is looking at.
    void refresh_extents();

    // Recomputes Style for every entity from the current impact result and filters.
    // Separated from sync() because changing a filter must restyle without touching
    // the graph or the layout.
    void restyle(const GraphStore& store);

    // Marks the entities along the currently explained path, for the renderer.
    void update_explained_path(const GraphStore& store);

    bool needs_layout_reset = true;

    struct Stats {
        int nodes = 0, edges = 0, visible_nodes = 0, visible_edges = 0;
        int changed = 0, impacted = 0, stale = 0;
        int muted = 0;   // impacted, but below the relevance threshold
    };
    Stats stats;

    // Architectural specificity over the current graph at the current level. Rebuilt
    // whenever styling is, which is cheap next to what it saves the reader.
    const analysis::SpecificityIndex& specificity() const { return specificity_; }

    // Changes to nodes most of the repository depends on. Empty almost always, and
    // the headline when it is not.
    const std::vector<analysis::HubAlert>& hub_alerts() const { return hub_alerts_; }

private:
    bool node_visible(const GraphStore& store, const Node& n) const;
    bool edge_visible(const GraphStore& store, const Edge& e) const;
    void upsert_node(const GraphStore& store, const Node& n);
    void upsert_edge(const GraphStore& store, const Edge& e);
    void drop_node(const NodeId& id);
    void drop_edge(const EdgeId& id);
    void seed_position(entt::entity e, const GraphStore& store, const Node& n);
    void refresh_specificity(const GraphStore& store);

    analysis::SpecificityIndex      specificity_;
    std::vector<analysis::HubAlert> hub_alerts_;

    std::unordered_map<NodeId, entt::entity> node_index_;
    std::unordered_map<EdgeId, entt::entity> edge_index_;
    ViewMode                                 built_mode_ = ViewMode::Architecture;
};

} // namespace rgv::ecs
