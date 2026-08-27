// Resolves what the pointer is over, once per frame.
//
// Hit-testing uses the same size function the renderer draws with, so a click can
// never land on a box that is no longer being drawn. Everything downstream reads
// PointerTarget rather than hit-testing again.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class PickingSystem final : public ecs::System {
public:
    std::string_view name() const override { return "PickingSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
