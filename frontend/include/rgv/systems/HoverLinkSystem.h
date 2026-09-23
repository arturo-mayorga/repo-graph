// Walks the dependency curves for whatever the pointer is on, once.
//
// The same argument FocusSystem makes: two readers need one answer, and the only way to
// guarantee they agree is to compute it in one place. Here the readers are the labels
// and the renderer -- one names the far end of a curve, the other draws it -- and they
// sit in different phases, so two walks could see two different hovers within a frame.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class HoverLinkSystem final : public ecs::System {
public:
    std::string_view name() const override { return "HoverLinkSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
