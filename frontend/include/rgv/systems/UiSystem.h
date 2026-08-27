// Owns the ImGui context and draws the panels.
//
// Runs first in the Render phase, because the panels decide how much of the window the
// graph gets and the renderer needs that rectangle.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

class UiSystem final : public ecs::System {
public:
    std::string_view name() const override { return "UiSystem"; }
    void             setup(ecs::World& world) override;
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
    void             teardown(ecs::World& world) override;
};

// Labels, legend, hover card, and the text-size window -- everything drawn over the
// graph rather than beside it. Separate from UiSystem because it must run after the
// graph has been drawn, not before.
class OverlaySystem final : public ecs::System {
public:
    std::string_view name() const override { return "OverlaySystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

// Hands the frame to the screen.
class PresentSystem final : public ecs::System {
public:
    std::string_view name() const override { return "PresentSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
};

} // namespace rgv::systems
