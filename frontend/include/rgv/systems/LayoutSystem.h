// Layout.
//
// Two strategies, picked by view mode:
//
//   * Architecture / File graph -- layered. Dependency depth fixes the row; ordering
//     within a row is solved by barycentre sweeps (the ordering phase of a Sugiyama
//     layout), then rows are spaced evenly. Deterministic, nothing to settle, and
//     legible at 240 packages. A spring simulation was tried first and produced a
//     hairball that never stopped drifting.
//
//   * Filesystem -- deterministic tidy tree.
//
// Rows run so that a package depending on nothing sits at the BOTTOM and its dependents
// stack above it: impact rises, the way the spec draws a blast radius.
//
// Owns Position, LayoutTarget, and Depth. Existing nodes seed the next ordering from
// where they already are, so adding a node cannot reshuffle the picture.
#pragma once

#include "rgv/ecs/System.h"

namespace rgv::systems {

struct LayoutParams {
    float layer_gap = 150.0f;   // minimum vertical distance between dependency layers
    float node_gap  = 34.0f;    // horizontal gap between neighbours in a row
    float ease      = 7.0f;     // higher converges faster; 0 disables animation
    int   sweeps    = 6;        // barycentre passes; more = fewer edge crossings
};

class LayoutSystem final : public ecs::System {
public:
    explicit LayoutSystem(LayoutParams params = {}) : params_(params) {}

    std::string_view name() const override { return "LayoutSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    void reset(ecs::World& world);
    void assign_depths(ecs::World& world);
    void order_and_place(ecs::World& world);
    void tidy_tree(ecs::World& world);

    LayoutParams params_;
    float        energy_     = 1e9f;
    int          depth_span_ = 1;
    bool         tree_mode_  = false;
};

} // namespace rgv::systems
