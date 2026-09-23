// Finds the dependency cycles in what is currently drawn, and marks them.
//
// It runs over the scene rather than over the store on purpose. The store knows every
// edge at every level at once; the question a reader is asking is about the boxes in
// front of them, and the answer differs by altitude -- two packages can be entangled
// while no two of their files are, and a ring between two modules inside one package is
// that package's business rather than the architecture's. Folding is what makes those
// different questions, so the fold has to happen first.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class CycleSystem final : public ecs::System {
public:
    std::string_view name() const override { return "CycleSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
