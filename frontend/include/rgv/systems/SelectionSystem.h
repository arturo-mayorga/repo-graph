// Projects the Selection resource onto entities.
//
// The only writer of Selected, Hovered, and OnExplainedPath. Previously those
// components and the selected id were maintained side by side at three call sites, and
// one of them -- selecting from the inspector -- set the id but not the component, so
// the canvas showed no outline. Deriving them from one source makes that class of bug
// unrepresentable.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class SelectionSystem final : public ecs::System {
public:
    std::string_view name() const override { return "SelectionSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
