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
//   * Filesystem -- a radial tree, inspired by Gource. Directories are discs whose
//     radius is set by how many files they hold, files ring the directory that owns
//     them, and subtrees splay outward into angular sectors. Collisions are prevented
//     by construction rather than by relaxation: each child gets a disjoint wedge and
//     is pushed far enough out that its whole subtree fits inside it.
//
// Rows run so that a package depending on nothing sits at the BOTTOM and its dependents
// stack above it: impact rises, the way the spec draws a blast radius.
//
// Owns Position, LayoutTarget, and Depth. Existing nodes seed the next ordering from
// where they already are, so adding a node cannot reshuffle the picture.
#pragma once

#include "rgv/ecs/System.h"

#include <cstdint>
#include <unordered_map>

namespace rgv::systems {

struct LayoutParams {
    float layer_gap = 150.0f;   // minimum distance between dependency rings
    float node_gap  = 34.0f;    // gap between neighbours sharing a ring
    int   max_rings = 7;        // reach is bucketed into at most this many rings
    float ease      = 7.0f;     // higher converges faster; 0 disables animation
    int   sweeps    = 6;        // barycentre passes; more = fewer edge crossings

    // Radial (filesystem) layout.
    //
    // A directory's DRAWN size and the ORBIT its files sit on are separate. Conflating
    // them put every file dot exactly on the directory's edge, half-occluding it, and
    // made the size ratio between a file and a directory the ratio of a dot to a whole
    // orbit -- far too stark to read as "the same kind of thing, one bigger".
    // Node sizes step by the golden ratio: a file, a directory, and the largest a
    // directory grows to are r, r*phi, r*phi^2. Three sizes on one geometric scale read
    // as a family -- clearly different, obviously related -- where an arbitrary ratio
    // reads as two unrelated shapes or as no difference at all.
    static constexpr float kGolden = 1.6180339887f;

    float file_radius     = 6.0f;                            // a file dot
    float dir_radius      = 6.0f * kGolden;                  // a directory holding nothing
    float dir_radius_max  = 6.0f * kGolden * kGolden;        // the largest one gets

    // Spacing is set relative to those sizes: shrinking the discs without shrinking the
    // gaps leaves the graph spatially large and every node a few pixels once fitted.
    float file_gap        = 6.0f;    // arc gap between files sharing an orbit
    float orbit_gap       = 5.0f;    // clearance between a directory and its file ring
    float dir_gap         = 12.0f;   // clearance between a subtree and its neighbours
    // How wide the first ring of children may get, as a multiple of the largest child.
    // Past this, children spill into further shells rather than one enormous ring.
    float shell_spread    = 5.0f;

    // Live relaxation, which runs only while a node is being dragged.
    //
    // The layout itself has no forces -- it is structural packing, deliberately, so
    // nothing drifts. But a drag wants the graph to give way and reflow, so dragging
    // switches on a spring-and-repulsion relaxation seeded from the packing: every
    // containment edge remembers the length the packing gave it, and that becomes its
    // rest length. The equilibrium of the simulation is the layout it started from.
    float relax_spring = 0.30f;   // how firmly a child holds its distance from its parent
    float relax_repel  = 0.55f;   // how firmly overlapping nodes push apart
    int   relax_iters  = 3;       // relaxation passes per frame

    // Releasing does not stop the relaxation -- it keeps running until the graph is
    // quiet, so a dropped node travels to a position consistent with everything around
    // it instead of being frozen wherever the cursor left it.
    float relax_quiet = 0.20f;    // per-frame movement below which the graph is at rest
    float relax_max   = 12.0f;    // seconds before giving up, so it can never run forever
};

class LayoutSystem final : public ecs::System {
public:
    explicit LayoutSystem(LayoutParams params = {}) : params_(params) {}

    std::string_view name() const override { return "LayoutSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    void reset(ecs::World& world);
    void assign_depths(ecs::World& world);
    void concentric_place(ecs::World& world);
    void radial_tree(ecs::World& world);
    void measure_spacing(ecs::World& world);
    void apply_drag(ecs::World& world);
    void relax(ecs::World& world, float dt);
    void relax_rings(ecs::World& world, float dt);
    bool seat_newcomers(ecs::World& world);
    void capture_rest_lengths(ecs::World& world);

    LayoutParams params_;
    float        energy_     = 1e9f;
    int          depth_span_ = 1;
    bool         tree_mode_  = false;

    // The radius each ring was placed at. A dependency drag relaxes within the rings
    // rather than freely: the ring is the reach reading, so a node that drifts off its
    // own stops telling the truth about how much of the repository is behind it.
    std::unordered_map<int, float> ring_radius_;

    // Live-relaxation state. `rest_` is captured when a drag starts, so the springs
    // pull toward what the structural layout produced rather than toward a guess.
    std::unordered_map<std::uint32_t, float> rest_;
    bool  relaxing_      = false;
    float relax_motion_  = 0.0f;   // largest displacement in the last pass
    float relax_elapsed_ = 0.0f;
};

} // namespace rgv::systems
