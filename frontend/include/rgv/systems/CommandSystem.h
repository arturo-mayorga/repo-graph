// Drains the command queue.
//
// Every system that wants something to happen elsewhere pushes a command; this is the
// one place that applies them. Selection in particular used to be written from three
// call sites, two of which forgot half the job.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class CommandSystem final : public ecs::System {
public:
    std::string_view name() const override { return "CommandSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
