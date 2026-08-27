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

namespace rgv::systems {

struct LayoutParams {
    float layer_gap = 150.0f;   // minimum vertical distance between dependency layers
    float node_gap  = 34.0f;    // horizontal gap between neighbours in a row
    float ease      = 7.0f;     // higher converges faster; 0 disables animation
    int   sweeps    = 6;        // barycentre passes; more = fewer edge crossings

    // Radial (filesystem) layout.
    //
    // A directory's DRAWN size and the ORBIT its files sit on are separate. Conflating
    // them put every file dot exactly on the directory's edge, half-occluding it, and
    // made the size ratio between a file and a directory the ratio of a dot to a whole
    // orbit -- far too stark to read as "the same kind of thing, one bigger".
    // Spacing is set relative to the node sizes: shrinking the discs without shrinking
    // the gaps leaves the graph spatially large and every node a few pixels once it is
    // fitted to the window.
    float file_radius     = 5.5f;    // a file dot
    float file_gap        = 6.0f;    // arc gap between files sharing an orbit
    float dir_radius      = 9.0f;    // a directory holding nothing
    float dir_radius_per  = 1.9f;    // added per sqrt(file), so size still means something
    float dir_radius_max  = 22.0f;   // a directory never dwarfs its own files
    float orbit_gap       = 5.0f;    // clearance between a directory and its file ring
    float dir_gap         = 12.0f;   // clearance between a subtree and its neighbours
    // How wide the first ring of children may get, as a multiple of the largest child.
    // Past this, children spill into further shells rather than one enormous ring.
    float shell_spread    = 5.0f;
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
    void radial_tree(ecs::World& world);

    LayoutParams params_;
    float        energy_     = 1e9f;
    int          depth_span_ = 1;
    bool         tree_mode_  = false;
};

} // namespace rgv::systems
