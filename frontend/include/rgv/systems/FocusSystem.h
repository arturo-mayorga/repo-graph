// How far everything on screen is from the node being looked at.
//
// One breadth-first walk, written down once, read by two systems that would otherwise
// each do their own: the layout keys its layers on it, and the style dims by it. They
// have to agree -- a node drawn on the second ring and lit like the fourth is a picture
// that contradicts itself -- and the only way to guarantee that is one owner.
//
// Over what is DRAWN, not over the store. The store knows every edge at every level at
// once; "near what I am looking at" is a question about the boxes in front of you, and
// the answer differs by altitude.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class FocusSystem final : public ecs::System {
public:
    std::string_view name() const override { return "FocusSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
