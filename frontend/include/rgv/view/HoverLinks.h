// Dependency curves for the containment views.
//
// The filesystem view draws containment and nothing else: discs for directories, files
// on their orbits, straight stubs from a child to the disc it belongs to. That is what
// makes it readable, and adding a few hundred dependency lines to it permanently would
// destroy exactly the quality worth keeping.
//
// So they are drawn for one node at a time, the one under the pointer, and they are
// drawn as curves. The curve is not decoration: every line the layout itself draws is a
// straight radial stub, so a bowed line cannot be mistaken for one. And because these
// are produced at draw time from the store rather than as entities, the layout never
// sees them -- hovering moves nothing.
#pragma once

#include "rgv/contract/Types.h"
#include "rgv/model/GraphStore.h"
#include "rgv/render/Math.h"

#include <functional>
#include <vector>

namespace rgv::view {

// One dependency of the hovered node, with the far end resolved to something drawn.
struct HoverLink {
    NodeId   other;              // the node at the far end, as it appears on screen
    bool     outgoing = true;    // true: the hovered node depends on `other`
    EdgeKind kind     = EdgeKind::Unknown;
    EdgeId   edge;               // the most specific contract edge behind it
};

// Every dependency crossing the boundary of `hovered`, counting what it contains: a
// directory answers for its whole subtree, which is what makes "what does this folder
// need" a hover rather than a query. Edges that stay entirely inside are not crossings
// and are dropped, as are far ends that resolve back inside.
//
// `on_screen` says whether a node is drawn; a far end that is not is resolved up the
// containment tree until one is, so a symbol answers as the file that defines it.
// One link survives per (far end, direction), the most specific of them.
std::vector<HoverLink> hover_links(const GraphStore& store, const NodeId& hovered,
                                   const std::function<bool(const NodeId&)>& on_screen);

// A quadratic Bezier from `a` to `b`, bowed toward `toward`, sampled into `segments`
// straight pieces. Bowing toward the hub of a radial layout makes the curves follow the
// tree they are drawn over instead of cutting across it.
//
// The deflection is capped at a fraction of the span, so a curve never swings wider
// than the distance it covers. Without that a short hop beside a distant hub arcs right
// across the view: the pull is a fraction of the distance to the hub, which has nothing
// to do with how far apart the two nodes are. `bend` is the fraction of the way to the
// hub; `max_span` is the cap as a fraction of the chord, and is what usually decides.
std::vector<Vec2> sample_bow(Vec2 a, Vec2 b, Vec2 toward, float bend, int segments,
                             float max_span = 0.35f);

} // namespace rgv::view
