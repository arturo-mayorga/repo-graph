#include "rgv/systems/SourceSystem.h"

#include "rgv/ecs/Resources.h"
#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"

namespace rgv::systems {

void SourceSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto& handle = world.resource<ecs::SourceHandle>();
    if (!handle.source) return;

    auto& store = world.resource<GraphStore>();

    // A newly attached source has to be seeded from its baseline before any event is
    // drained, or the first delta lands on an empty graph. This used to happen only
    // through the fixture reset path below, which meant a live source silently produced
    // nothing: it has no timeline, so nothing ever asked it for its baseline.
    //
    // Pointer comparison rather than a type test, so swapping fixture sets and attaching
    // a watcher take the same path.
    if (seeded_ != handle.source) {
        seeded_ = handle.source;
        store.reset(handle.source->baseline());
        world.resource<ecs::SceneRequests>().rebuild  = true;
        world.resource<ecs::CameraControl>().auto_fit = true;
    }

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
