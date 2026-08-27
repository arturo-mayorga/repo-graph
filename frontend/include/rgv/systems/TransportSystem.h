// Keyboard control of a replayable source: play/pause, step, restart.
//
// Does nothing when the attached source has no timeline, which is how a live backend
// will behave. The keys simply stop responding rather than the app needing to know
// what kind of source it has.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class TransportSystem final : public ecs::System {
public:
    std::string_view name() const override { return "TransportSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
