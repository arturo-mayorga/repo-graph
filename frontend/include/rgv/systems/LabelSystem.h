// Which nodes get a name beside them, and how it fades in and out.
//
// A label beside a node holds a constant screen size while the graph spreads out under
// it, so at any zoom there is a fixed amount of room and more names than room. This
// decides who gets it: priority order, and a name is drawn only if it clears every name
// already drawn. Priority is how busy the node is -- its in-degree plus its out-degree
// over the edges actually on screen -- so the names that survive are the ones the most
// of the picture connects to. What the user is pointing at outranks all of it.
//
// It is a system rather than something the panel works out as it draws, for two
// reasons. The choice depends on every other label on screen, so it cannot be made one
// node at a time. And the fade has to remember what it was last frame.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class LabelSystem final : public ecs::System {
public:
    std::string_view name() const override { return "LabelSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
