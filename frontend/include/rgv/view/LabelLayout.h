// Which of the labels drawn beside a node actually get drawn.
//
// A name beside a node is the one thing in the picture that does not scale with the
// graph: it holds a constant screen size while the nodes spread out under it, so at any
// zoom there is a fixed amount of room and more names than room. Something has to
// choose, and the choice should be the same every frame for the same picture.
//
// So: sort by priority, take them in order, and keep one only if it clears everything
// already kept. Greedy rather than optimal on purpose -- an optimal packing would
// reshuffle wholesale when one node moves, and a label that jumps to a different node
// is worse than a label that is missing.
#pragma once

#include "rgv/render/Math.h"

#include <vector>

namespace rgv::view {

// One label competing for room, in screen space.
struct LabelBox {
    Vec2  min;
    Vec2  max;
    float priority = 0.0f;
};

// The indices of the labels that are drawn, in the order they were chosen. Ties break
// on index so the same picture always resolves the same way.
std::vector<int> choose_labels(const std::vector<LabelBox>& boxes);

// What a node's label is worth, out of how much is attached to it and what the user is
// doing with it.
struct LabelRank {
    int  degree   = 0;       // dependency degree, plus what it holds on screen
    bool on_path  = false;   // on the dependency path being explained
    bool linked   = false;   // a curve from the node under the pointer arrives here
    bool selected = false;
    bool hovered  = false;
};

// Bands, not a sum. Attention is a ladder -- hovered above selected above the curves
// that hover drew above the explanation above the graph's own shape -- and a sum would
// let a node that is both selected and linked climb over the one being pointed at.
// Degree orders nodes within a band and never between them, so a node keeps its place
// relative to its equals when the whole band is promoted.
float label_priority(const LabelRank& rank);

} // namespace rgv::view
