// Derives architectural specificity (an IDF over in-degree) and any hub-change alert.
//
// Runs in Sync rather than Simulate because it is derived from the graph store, not
// from entities -- and because SceneSyncSystem's visibility filter consults it.
#pragma once

#include "rgv/contract/Types.h"
#include "rgv/ecs/System.h"

namespace rgv::systems {

class SpecificitySystem final : public ecs::System {
public:
    std::string_view name() const override { return "SpecificitySystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    // Rebuilding is linear in the graph, so it is gated on the inputs actually
    // changing rather than run every frame.
    Generation last_generation_ = 0;
    Level      last_level_      = Level::Package;
    bool       last_heuristic_  = true;
    bool       primed_          = false;
};

} // namespace rgv::systems
