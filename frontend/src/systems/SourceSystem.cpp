#include "rgv/systems/SourceSystem.h"

#include "rgv/ecs/Resources.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"

namespace rgv::systems {

void SourceSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto& handle = world.resource<ecs::SourceHandle>();
    if (!handle.source) return;

    auto& store = world.resource<GraphStore>();

    // A replayable source rewinds when the timeline is scrubbed or the scenario
    // changes. Re-seeding from the baseline here, before any events are drained, is
    // what makes a seek land in exactly the state playing forward would have produced.
    if (handle.fixtures && handle.fixtures->take_reset()) {
        store.reset(handle.source->baseline());
        world.resource<ecs::SceneRequests>().rebuild = true;
        world.resource<ecs::CameraControl>().auto_fit = true;
    }

    handle.source->poll(frame.dt, store);
}

} // namespace rgv::systems
