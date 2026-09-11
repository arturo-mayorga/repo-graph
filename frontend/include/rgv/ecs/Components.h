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

// Endpoint entities, resolved once at sync time so nothing does a string lookup per
// frame. Owner: SceneSyncSystem.
struct Endpoints {
    entt::entity from = entt::null;
    entt::entity to   = entt::null;
};

// -- spatial ------------------------------------------------------------------

// Owner: LayoutSystem. Seeded once at creation near whatever the node connects to,
// so a package appearing mid-session does not fly in from the origin.
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

// A node drawn as a container: what it holds is laid out inside it, and this is the
// size that needs. World-space and never collapsed to a dot -- a package that is a box
// around its modules stays that box at every zoom, the way a directory disc does.
// Present only on nodes with something inside them. Owner: LayoutSystem.
struct Hull {
    Vec2  half{60.0f, 40.0f};
    float header = 26.0f;   // room at the top for the container's own name
};

// Drawn at its layout footprint at every zoom, like a disc, rather than collapsing to a
// dot. Present on what is laid out inside a container: the container keeps its world
// size, so a module inside it must too, or a package at overview is a box with a few
// dots floating in it instead of a box full of modules. Owner: LayoutSystem.
struct WorldBox {};

// Dependency depth: 0 = depends on nothing else in view. Owner: LayoutSystem.
struct Depth { int value = 0; };

// Where a node sits in the concentric dependency layout. Ring 0 is the core -- the
// nodes the most of the repository transitively depends on -- and the index rises
// outward as reach falls, so consumers end up on the rim.
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

// The user dragged this node. Layout leaves it alone. Owner: DragSystem.
// A node that has appeared but has not been given a place yet. Layout claims these,
// seats them next to whatever they are connected to, and clears the tag -- which is how
// a filter change costs one node's placement instead of the whole graph's.
// Written by SceneSyncSystem on creation, cleared by LayoutSystem.
struct Unplaced {};

struct Pinned {};

// -- presentation -------------------------------------------------------------

// Owner: SceneSyncSystem.
struct Label {
    std::string text;
    std::string sub;   // secondary line: package path, language
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

} // namespace rgv::ecs
