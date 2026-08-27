// Discovering fixture sets on disk and attaching one as the world's data source.
//
// The only part of the application that knows what a fixture is. Everything else --
// including the command that swaps between them -- goes through IGraphSource and the
// FixtureLibrary resource. Replacing this file with a watcher-backed source is the
// intended path to live data.
#pragma once

#include "rgv/ecs/World.h"

#include <string>

namespace rgv::app {

// Installs SourceOwner and FixtureLibrary, discovers sets under `root`, and attaches
// one. `preferred` names a set; when empty the smallest is chosen, because opening on
// a 240-package stress graph teaches nothing about the interaction.
// Returns false when nothing usable was found.
bool attach_fixture_source(ecs::World& world, const std::string& root,
                           const std::string& preferred, int scenario);

} // namespace rgv::app
