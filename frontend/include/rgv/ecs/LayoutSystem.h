// Layout.
//
// Two strategies, picked by view mode:
//
//   * Architecture / File graph -- layered. Dependency depth fixes the row; ordering
//     within a row is solved by barycentre sweeps (the ordering phase of a Sugiyama
//     layout), then rows are spaced evenly. Deterministic, no energy to settle, and
//     legible at 240 packages. A spring simulation was tried first and produced a
//     hairball that never stopped drifting.
//
//   * Filesystem -- deterministic tidy tree.
//
// Rows run so that a package depending on nothing sits at the BOTTOM and its
// dependents stack above it, matching how the spec draws a blast radius: impact rises.
//
// Both are incremental: existing nodes seed the next ordering from where they already
// are, so adding a node cannot reshuffle the picture. Positions ease toward targets
// rather than snapping, which is what makes a mid-session topology change readable.
#pragma once

#include "rgv/ecs/Scene.h"
#include "rgv/model/GraphStore.h"

namespace rgv::ecs {

struct LayoutParams {
    float layer_gap = 150.0f;   // vertical distance between dependency layers
    float node_gap  = 34.0f;    // horizontal gap between neighbours in a row
    float ease      = 7.0f;     // higher converges faster; 0 disables animation
    int   sweeps    = 6;        // barycentre passes; more = fewer edge crossings
};

class LayoutSystem {
public:
    LayoutParams params;

    // Recomputes depths, row ordering, and target positions.
    void reset(Scene& scene, const GraphStore& store);

    // Eases positions toward their targets. Cheap and idempotent once converged.
    void step(Scene& scene, float dt);

    bool  settled() const { return energy_ < 0.5f; }
    float energy() const { return energy_; }
    int   depth_span() const { return depth_span_; }

private:
    void assign_depths(Scene& scene);
    void order_and_place(Scene& scene);
    void tidy_tree(Scene& scene);

    float energy_     = 1e9f;
    int   depth_span_ = 1;
    bool  tree_mode_  = false;
};

} // namespace rgv::ecs
