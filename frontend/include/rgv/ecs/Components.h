// ECS components. Everything the renderer and the interaction systems need lives in
// components; nothing reaches back into GraphStore during a frame.
#pragma once

#include "rgv/contract/Types.h"
#include "rgv/render/Math.h"

#include <string>

namespace rgv::ecs {

// -- identity ----------------------------------------------------------------

struct NodeRef {
    NodeId   id;
    NodeKind kind = NodeKind::Unknown;
};

struct EdgeRef {
    EdgeId   id;
    EdgeKind kind = EdgeKind::Unknown;
};

// Endpoint entities, resolved once at sync time so layout and rendering never do a
// string lookup per frame.
struct Endpoints {
    std::uint32_t from = 0;   // entt::entity, stored raw to keep this header light
    std::uint32_t to   = 0;
};

// -- spatial -----------------------------------------------------------------

struct Position { Vec2 p; };
struct Velocity { Vec2 v; };
struct Extent   { Vec2 half{54.0f, 17.0f}; };

// Dependency depth: 0 = depends on nothing else in view. Drives the layered layout,
// which is what turns a hairball into something that reads as an architecture.
struct Depth { int value = 0; };

// The user dragged this node. Layout must leave it alone until they release it.
struct Pinned {};

// Where layout wants this node. Positions ease toward it, so a topology change
// animates instead of teleporting -- and the user can watch what moved.
struct LayoutTarget { Vec2 p; };

// -- presentation ------------------------------------------------------------

struct Label {
    std::string text;
    std::string sub;   // secondary line: package path, language, distance
};

struct Style {
    Vec4  fill{0.16f, 0.17f, 0.20f, 1.0f};
    Vec4  stroke{0.30f, 0.32f, 0.38f, 1.0f};
    float stroke_w = 1.5f;
    float dash     = 0.0f;   // >0 => dashed, in world units per cycle
    float emphasis = 0.0f;   // 0 = context, 1 = fully lit
};

// -- semantic state ----------------------------------------------------------

struct FreshnessState { Freshness value = Freshness::Current; };
struct ConfidenceState { Confidence value = Confidence::Exact; };

// Present only on nodes the agent actually touched.
struct Changed {
    FileChangeKind kind       = FileChangeKind::Modified;
    Processing     processing = Processing::Pending;
};

// Present only on nodes inside the current blast radius.
struct Impacted {
    int         distance = 0;
    bool        direct   = false;
    ImpactCause cause    = ImpactCause::Implementation;

    // How much this node's presence in the radius actually tells you: the weakest
    // architectural specificity along its explanation. A path through a hub scores
    // low, because "everything depends on the hub" was already known.
    float relevance = 1.0f;

    // Below the user's relevance threshold. Still impacted, still true -- just not
    // worth their attention, so it is drawn as context.
    bool muted = false;
};

// The agent changed something most of the repository depends on. Rare, and the loudest
// thing the product can say.
struct HubSeed {
    int   dependents     = 0;
    int   population     = 0;
    float specificity    = 0.0f;
    float reach_fraction = 0.0f;
};

// -- transient interaction ---------------------------------------------------

struct Hovered {};
struct Selected {};
// On the dependency path currently being explained in the inspector.
struct OnExplainedPath { int hop = 0; };
// Filtered out, but kept alive so its layout position survives the filter toggling.
struct Hidden {};

} // namespace rgv::ecs
