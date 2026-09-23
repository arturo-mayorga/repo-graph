// Components: per-entity state, iterated in bulk by systems.
//
// Two archetypes share the registry. A node entity carries NodeRef; an edge entity
// carries EdgeRef and Endpoints. Those are the tags -- systems name them explicitly in
// their views rather than relying on which components an archetype happens to lack.
//
// Every component names the system that owns its VALUE, and no other system writes it.
// That rule is the difference between components as data and components as a shared
// scratchpad -- and it is what makes "why is this node the wrong colour" a question
// with one place to look.
//
// One deliberate exception: SceneSyncSystem creates entities, and creation means
// attaching the whole archetype at once -- including components it does not own, in
// their default state. Constructing is not owning. After the entity exists, only the
// named owner writes the value.
#pragma once

#include "rgv/contract/Types.h"
#include "rgv/render/Math.h"

#include <entt/entt.hpp>

#include <string>

namespace rgv::ecs {

// -- identity: also the archetype tags ---------------------------------------

// Owner: SceneSyncSystem.
struct NodeRef {
    NodeId   id;
    NodeKind kind = NodeKind::Unknown;
};

// Owner: SceneSyncSystem.
struct EdgeRef {
    EdgeId   id;
    EdgeKind kind = EdgeKind::Unknown;
};

// How many contract edges this one line stands for. Several land on the same pair of
// nodes once the view aggregates -- the import of a module and every read of a symbol
// inside it -- and a diagram draws one line, not twelve. Owner: SceneSyncSystem.
struct EdgeWeight { int count = 1; };

// Endpoint entities, resolved once at sync time so nothing does a string lookup per
// frame. Owner: SceneSyncSystem.
struct Endpoints {
    entt::entity from = entt::null;
    entt::entity to   = entt::null;
};

// -- spatial ------------------------------------------------------------------

// Owner: LayoutSystem, and only LayoutSystem -- including the seed an arrival gets
// before it is placed, so it eases in from beside whatever it connects to rather than
// flying in from the origin. SceneSyncSystem creates the entity without this; placing
// things is not a construction detail.
struct Position { Vec2 p; };

// Where layout wants this node. Positions ease toward it, so a topology change
// animates instead of teleporting. Owner: LayoutSystem.
struct LayoutTarget { Vec2 p; };

// The footprint layout reserves. Deliberately independent of zoom: the drawn size
// collapses toward a dot at low zoom, but this does not, so panning and zooming can
// never reflow the graph. Owner: SceneSyncSystem.
struct Extent { Vec2 half{54.0f, 17.0f}; };

// How much of the repository depends on this node, as a size multiplier on the golden
// ratio: 1, phi, phi^2. The box views have no equivalent of the radial view's disc
// radius -- their footprint is whatever their name needs -- so without this a hub that
// half the repository imports is drawn exactly like a leaf nothing depends on, and the
// blast radius the product exists to show is invisible until something changes.
//
// Scales up only. Shrinking below the text's own footprint would trade legibility for
// a distinction the colour and the layout already carry. Owner: SceneSyncSystem.
struct Prominence { float scale = 1.0f; };

// Where a node sits in the concentric dependency layout, under whichever key the
// arrangement is currently using. Nothing focused: ring 0 is the core -- what the most
// of the repository transitively depends on -- and the index rises outward as reach
// falls, so consumers end up on the rim. Something focused: ring 0 is the selection
// and the index IS the hop count, so the rings read as distance from the question.
//
// Two meanings in one field is a hazard and it has already bitten once, so the key is
// not inferable from here: `LayoutSystem::ring_index_for` is the single place that
// knows which is live, and both the placer and the incremental seat ask it rather
// than computing their own. Do not add a third caller that computes its own.
//
// Polar coordinates are kept alongside the position because a drag has to relax in
// them: the ring is the reading, so radius springs home while the angle is free, the
// way the layered version sprang y home and left x alone. Owner: LayoutSystem.
struct Ring {
    int   index  = 0;
    float radius = 0.0f;
    float angle  = 0.0f;
};

// Distance to the nearest neighbouring node, measured after placement. Every view has
// one, because every view has to answer the same two questions: may this node grow into
// a labelled box, and is there space beside it to write its name? Owner: LayoutSystem.
struct Spacing { float room = 1e9f; };

// Present only where the view lays nodes out as circles rather than boxes -- the
// filesystem view. `radius` is the world-space radius of the disc itself; for a
// directory it is the ring its files sit on, so a directory's size IS its file count.
// Owner: LayoutSystem.
struct Disc {
    float radius = 6.0f;
    // Radius of everything this node holds: the outermost file orbit plus a dot. A
    // label has to clear this, not just the disc, or a directory's own name lands on
    // top of its files.
    float halo = 6.0f;
    // Unit vector pointing away from whatever this node orbits. Labels are placed
    // along it, so the names around a ring fan outward instead of stacking on top of
    // one another. Zero for a node with no parent.
    Vec2 outward{0.0f, 1.0f};
};

// What shape this node is DRAWN as, and the geometry drawing needs.
//
// The view decides this, not the layout. It used to be inferred from the presence of
// `Disc` by five separate consumers, which made a layout change the silent channel for
// restyling an entire view. Now one system reads the mode once and writes this, and
// everything downstream switches on `form` -- so "why is this node a circle" has one
// answer and `NodeShape` greps to every consumer. `Disc` goes back to being the
// packing geometry it is, read by layout and by this system and nobody else.
//
// `radius` is the world-space radius of the shape itself; `halo` is the reach of what
// orbits it, which a label has to clear; `outward` is the direction it orbits away
// from, so names around a ring fan out instead of stacking. All zero for a box.
// Owner: ShapeSystem.
struct NodeShape {
    enum class Form { Box, Disc };
    Form  form   = Form::Box;
    float radius = 0.0f;
    float halo   = 0.0f;
    Vec2  outward{0.0f, 1.0f};
};

// A node that has appeared but has not been given a place yet. Layout claims these,
// seats them next to whatever they are connected to, and clears the tag -- which is how
// a filter change costs one node's placement instead of the whole graph's.
// Written by SceneSyncSystem on creation, cleared by LayoutSystem.
struct Unplaced {};

// Held where the user put it: layout proposes a place, this refuses it. Owner:
// CommandSystem, so there is one place the pin can change and one order it happens in.
struct Pinned {};

// -- presentation -------------------------------------------------------------

// Owner: SceneSyncSystem.
struct Label {
    std::string text;
    std::string sub;   // secondary line: package path, language
};

// A name drawn BESIDE a node rather than inside it, once the label system has decided
// it gets one. Present only on nodes whose name is currently drawn or fading; where it
// is absent the node is either off screen or has grown into a box that holds its own
// name. Owner: LabelSystem.
//
// The anchor is screen space and the alpha is the fade, which is why this is a
// component and not something the panel works out as it draws: the choice depends on
// every other label on screen, and the fade has to remember what it was last frame.
struct SideLabel {
    Vec2  anchor;         // horizontal centre of the text, at its top
    float px    = 0.0f;   // font size, constant on screen
    float alpha = 0.0f;
};

// Fully derived from the state below plus Selected/Hovered/OnExplainedPath.
// Owner: StyleSystem, and nothing else may write it -- the renderer reads it verbatim.
struct Style {
    Vec4  fill{0.16f, 0.17f, 0.20f, 1.0f};
    Vec4  stroke{0.30f, 0.32f, 0.38f, 1.0f};
    float stroke_w = 1.5f;
    float dash     = 0.0f;   // >0 => dashed, in world units per cycle
    float emphasis = 0.0f;   // 0 = context, 1 = fully lit
};

// -- evidence quality ---------------------------------------------------------

// Owner: SceneSyncSystem.
struct FreshnessState { Freshness value = Freshness::Current; };
struct ConfidenceState { Confidence value = Confidence::Exact; };

// -- impact -------------------------------------------------------------------

// Present only on nodes the agent actually touched. Owner: ImpactStateSystem.
struct Changed {
    FileChangeKind kind       = FileChangeKind::Modified;
    Processing     processing = Processing::Pending;
};

// Present only on nodes inside the current blast radius. Owner: ImpactStateSystem.
struct Impacted {
    int         distance = 0;
    bool        direct   = false;
    ImpactCause cause    = ImpactCause::Implementation;

    // The weakest architectural specificity along this node's explanation. A path
    // through a hub scores low: "everything depends on the hub" was already known.
    float relevance = 1.0f;

    // As reported by the backend: how trustworthy the PATH is, which can be worse than
    // anything the node says about itself.
    Freshness freshness = Freshness::Current;

    // Below the user's relevance threshold. Still impacted and still true, just not
    // worth their attention, so it is drawn as context.
    bool muted = false;
};

// The agent changed something most of the repository depends on. Rare, and the loudest
// thing the product can say. Owner: ImpactStateSystem.
struct HubSeed {
    int   dependents     = 0;
    int   population     = 0;
    float specificity    = 0.0f;
    float reach_fraction = 0.0f;
};

// -- interaction: all derived from the Selection resource ---------------------
// Owner: SelectionSystem. Nothing else writes these.

struct Hovered {};
struct Selected {};

// On the dependency path currently being explained in the inspector.
struct OnExplainedPath { int hop = 0; };

// -- focus --------------------------------------------------------------------

// Hops from the focused node, over the dependency graph read as UNDIRECTED. Direction
// is the right question for "what breaks if I change this"; it is the wrong one for
// "what is near what I am looking at", where a thing that imports me is exactly as
// close as a thing I import.
//
// Present on every node and every line whenever something is focused, absent on all of
// them when nothing is. `hops` is `kUnreached` for what the focus cannot reach at all
// -- never absent, because the whole point is that nothing is hidden and unreachable
// is a legitimate, and dim, answer. Owner: FocusSystem.
inline constexpr int kUnreached = 1 << 20;
struct FocusDistance { int hops = 0; };

// -- findings -----------------------------------------------------------------

// This node, or this line, is inside a dependency cycle: every other member of the
// group reaches it and it reaches them. `group` indexes `CycleReport::groups`.
// Absent on everything that is in no cycle, which is the ordinary case and why this is
// a tag to look for rather than a field to check. Owner: CycleSystem.
struct InCycle { int group = 0; };

} // namespace rgv::ecs
