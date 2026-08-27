#include "rgv/systems/TransportSystem.h"

#include "rgv/ecs/Resources.h"

namespace rgv::systems {

void TransportSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto& handle = world.resource<ecs::SourceHandle>();
    if (!handle.source) return;

    Timeline* timeline = handle.source->timeline();
    if (!timeline) return;   // a live source has no transport, and that is fine

    const auto& input = world.resource<ecs::FrameInput>();
    if (input.play_pressed) timeline->playing() ? timeline->pause() : timeline->play();
    if (input.step_pressed) timeline->step_event();
    if (input.restart_pressed) timeline->restart();
}

} // namespace rgv::systems
