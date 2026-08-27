// The only system that touches the operating system.
//
// Creates the window and the GL context, then each frame turns devices into the
// FrameInput resource. Everything downstream reads that resource, which is why picking,
// navigation, and the panels can all be exercised in a test with no window at all.
#pragma once

#include "rgv/ecs/System.h"
#include "rgv/render/Math.h"

#include <string>

namespace rgv::systems {

struct WindowConfig {
    int         width      = 1680;
    int         height     = 1000;
    std::string title      = "rgv - live repository impact graph";
    bool        fullscreen = false;
    bool        vsync      = true;
};

class WindowSystem final : public ecs::System {
public:
    explicit WindowSystem(WindowConfig config) : config_(std::move(config)) {}

    std::string_view name() const override { return "WindowSystem"; }
    void             setup(ecs::World& world) override;
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
    void             teardown(ecs::World& world) override;

private:
    WindowConfig config_;
    Vec2         last_mouse{0.0f, 0.0f};
    double       last_click_time_ = -1.0;
    bool         was_down_        = false;
};

} // namespace rgv::systems
