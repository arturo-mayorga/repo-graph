// Decides what shape each node is drawn as.
//
// The one place the view mode becomes a presentation fact. Containment is drawn as
// circles -- discs that grow with what they hold, files on their orbits -- because that
// is what makes a tree read as structure. The dependency views draw boxes, because what
// matters there is a name and an arrow, not how much a node contains.
//
// It exists as its own system rather than as a line inside the renderer because five
// consumers need the same answer: style, labels, picking, the renderer and the overlay.
// When they each inferred it from the presence of a layout component, a one-line change
// to the layout restyled a whole view without touching any of them.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class ShapeSystem final : public ecs::System {
public:
    std::string_view name() const override { return "ShapeSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
