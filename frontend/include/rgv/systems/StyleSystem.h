// Derives Style from everything else.
//
// The sole writer of Style, and the reason the renderer is dumb. Style used to be
// written here and then overridden again at draw time for selection, hover, and the
// explained path -- two places computing the same thing, with the second silently
// winning. Now the renderer reads it verbatim.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class StyleSystem final : public ecs::System {
public:
    std::string_view name() const override { return "StyleSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
