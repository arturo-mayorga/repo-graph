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

} // namespace rgv::view
