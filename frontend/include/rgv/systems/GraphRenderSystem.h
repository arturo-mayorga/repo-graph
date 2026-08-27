// Draws the graph.
//
// Deliberately dumb: it reads Style verbatim and never decides what anything should
// look like. Everything visual is derived upstream by StyleSystem, so there is exactly
// one place to look when a node is the wrong colour.
#pragma once

#include "rgv/ecs/System.h"
#include "rgv/render/GraphRenderer.h"

namespace rgv::systems {

class GraphRenderSystem final : public ecs::System {
public:
    std::string_view name() const override { return "GraphRenderSystem"; }
    void             setup(ecs::World& world) override;
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;
    void             teardown(ecs::World& world) override;

private:
    render::GraphRenderer renderer_;
};

} // namespace rgv::systems
