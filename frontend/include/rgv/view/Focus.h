// How bright a thing is, by how far it sits from what is focused.
//
// Shared because three places read it -- the node and edge styling, and the labels --
// and a node drawn dim with its name at full strength is not a dimmer node, it is a
// node with a loud label. The falloff has to be one rule or it is no rule.
//
// Geometric, not linear: each step out is a fixed fraction of the one before, so the
// first two or three hops separate sharply -- which is where the question is -- and the
// far field compresses instead of marching evenly into the background.
//
// Floored, and the floor is the whole design. This says "further away", never "not
// here": the dimmest thing in the graph is still findable, readable and clickable.
// Nothing is hidden, because an agent may change anything at any moment and a view that
// had quietly dropped it would be lying by omission.
#pragma once

#include "rgv/ecs/Components.h"

#include <algorithm>

namespace rgv::view {

inline constexpr float kFocusFalloff = 0.70f;
inline constexpr float kFocusFloor   = 0.22f;

// Text needs a higher floor than a dot does. A shape at 22% is still plainly a shape;
// letterforms at 22% on this background stop resolving, and an unreadable name is a
// hidden node wearing a smudge.
inline constexpr float kFocusTextFloor = 0.42f;

// `hops` from ecs::FocusDistance. Unreached sits at the floor with the far field rather
// than below it -- being in another part of the repository is a legitimate answer, not
// a lesser one.
inline float focus_brightness(int hops, float floor = kFocusFloor) {
    if (hops == ecs::kUnreached) return floor;
    float b = 1.0f;
    for (int i = 0; i < hops && b > floor; ++i) b *= kFocusFalloff;
    return std::max(b, floor);
}

// Nothing focused -- the component is absent -- means everything is at full strength.
inline float focus_brightness(const ecs::FocusDistance* fd, float floor = kFocusFloor) {
    return fd ? focus_brightness(fd->hops, floor) : 1.0f;
}

} // namespace rgv::view
