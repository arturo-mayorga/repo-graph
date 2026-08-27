// Attaches a live provider process as the graph source.
//
// The mirror of FixtureBootstrap, and deliberately the same shape: both hand a
// SourceHandle an IGraphSource and nothing downstream can tell which it got. The only
// visible difference is that a live source offers no Timeline, so no scrubber is drawn.
#pragma once

#include "rgv/ecs/World.h"

#include <string>

namespace rgv::app {

// `provider` is the command to run; `root` is the directory it should watch. Returns
// false and explains itself on stderr if the provider cannot be started or never sends
// a baseline.
bool attach_live_source(ecs::World& world, const std::string& provider,
                        const std::string& root);

} // namespace rgv::app
