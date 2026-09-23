// Control of a replayable source: play/pause, step, restart, seek, rate.
//
// The sole owner of the timeline. Keys reach it as FrameInput and the transport panel
// reaches it as commands, which this system drains itself rather than leaving to
// CommandSystem -- the panel used to call play() and seek_ms() directly, and a second
// caller is how "who moved the timeline" stops having an answer.
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
