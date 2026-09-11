// Layout.
//
// Two strategies, picked by view mode:
//
//   * File graph -- concentric. Reach fixes the ring; ordering within a ring is
//     solved by barycentre sweeps (the ordering phase of a Sugiyama layout). Deterministic,
//     nothing to settle, and legible at thousands of files. A spring simulation was
//     tried first and produced a hairball that never stopped drifting.
//
//   * Architecture -- force-directed. A package is a node with containment links to
//     the modules and packages inside it; those links attract, everything repels, and
//     dependency edges pull weakly. Position-based with no velocity, cooled over the
//     passes, seeded deterministically and stopped when quiet, which is what keeps it
//     from being the hairball that never stopped drifting -- the reason the box views
//     were not force-directed in the first place. Boxes drawn around modules were tried
//     instead and got in the way of panning; this is what was asked for.
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
#include <unordered_set>

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
    // Force-directed architecture layout.
    float force_link      = 16.0f;    // clearance a module keeps from its package
    float force_dep       = 260.0f;   // preferred length of a dependency edge
    float force_dep_pull  = 0.0f;     // dependency edges are drawn, not pulled: containment is the structure
    float force_range     = 700.0f;   // beyond this, nodes do not repel
    float force_spread    = 30.0f;    // repulsion strength (k in k^2/d) between kin ...
    float force_apart     = 230.0f;   // ... and between nodes of different packages
    float force_gravity   = 0.0004f;  // pull toward the origin, so components stay together
    // Reading direction. A package depending on nothing sits at the bottom and its
    // dependents stack above it, so impact rises and the eye can follow it without a
    // legend. Crossings barely move at diagram size -- 38 against 40 on this repo --
    // but a graph with no direction cannot be read as a flow at all.
    float force_rank      = 0.20f;    // how firmly a node is held to its rank's row
    float force_rank_gap  = 230.0f;   // distance between rows
    int   force_passes    = 600;      // at most; it stops when quiet

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
    void force_place(ecs::World& world);
    // `gentle`: springs and overlap only -- no spreading, no gravity -- for the settling
    // that follows a newcomer or a resize, where the rule is that nothing else moves.
    void force_relax(ecs::World& world, float dt, int passes, float cooling, bool gentle);
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
    bool         force_mode_ = false;   // architecture: force-directed over containment

    // The radius each ring was placed at. A dependency drag relaxes within the rings
    // rather than freely: the ring is the reach reading, so a node that drifts off its
    // own stops telling the truth about how much of the repository is behind it.
    std::unordered_map<int, float> ring_radius_;

    // Live-relaxation state. `rest_` is captured when a drag starts, so the springs
    // pull toward what the structural layout produced rather than toward a guess.
    std::unordered_map<std::uint32_t, float> rest_;
    bool  relaxing_      = false;
    // Nodes seated since the last full layout and still finding their place. While a
    // non-drag relaxation runs, only these move (spec 11.2: a save must not reshuffle).
    std::unordered_set<std::uint32_t> loose_;
    float relax_motion_  = 0.0f;   // largest displacement in the last pass
    float relax_elapsed_ = 0.0f;
};

} // namespace rgv::systems
