// Pan, zoom, node dragging, and keeping the graph framed.
//
// Reads PointerTarget rather than hit-testing again, so "was the press on a node"
// is decided in exactly one place and pan and drag can never both claim a gesture.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class NavigationSystem final : public ecs::System {
public:
    std::string_view name() const override { return "NavigationSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    entt::entity dragging_ = entt::null;
    bool         panning_  = false;
};

} // namespace rgv::systems
