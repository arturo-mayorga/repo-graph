// Systems and the schedule that runs them.
//
// Every system has the same shape: it is handed the world and the frame, and it reads
// and writes components and resources. Nothing else. That uniformity is what makes the
// frame loop a list instead of a script, and what makes "attach a different source" a
// one-line change at the call site rather than an edit to main().
//
// Ordering is explicit and coarse-grained. Six phases, and insertion order within a
// phase. A finer dependency graph would buy parallelism this workload does not need
// and cost the ability to read the frame top to bottom.
#pragma once

#include "rgv/ecs/World.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rgv::ecs {

enum class Phase {
    // Turn devices and the OS into resources. Nothing else may read the platform.
    Input,
    // Bring the outside world in: replayed fixtures today, a watcher or a socket
    // later. The only phase permitted to mutate the graph store.
    Ingest,
    // Reconcile that data into entities and components.
    Sync,
    // Derive everything else: specificity, impact state, layout, style.
    Simulate,
    // Draw. Panels first, because they decide how much room the graph gets.
    Render,
    // Hand the frame to the screen.
    Present,
};

std::string_view to_string(Phase p);

struct FrameContext {
    float         dt      = 0.0f;   // seconds since the previous frame, clamped
    double        time    = 0.0;    // seconds since startup
    std::uint64_t index   = 0;      // frame counter
};

class System {
public:
    virtual ~System() = default;

    // For diagnostics, the schedule listing, and tests. Must be stable.
    virtual std::string_view name() const = 0;

    // Called once, in phase order, before the first frame. The place to install the
    // resources a system owns, so wiring failures surface at startup.
    virtual void setup(World&) {}

    virtual void run(World&, const FrameContext&) = 0;

    // Called once, in reverse order, after the last frame.
    virtual void teardown(World&) {}
};

class Schedule {
public:
    // Returns *this so a schedule reads as a declaration of what the app is.
    Schedule& add(Phase phase, std::unique_ptr<System> system);

    void setup(World& world);
    void run(World& world, const FrameContext& frame);
    void teardown(World& world);

    // "Input/WindowSystem", "Ingest/SourceSystem", ... in execution order. Exposed so
    // the wiring can be asserted in a test rather than trusted.
    std::vector<std::string> listing() const;

    std::size_t size() const { return systems_.size(); }

private:
    struct Entry {
        Phase                   phase;
        std::unique_ptr<System> system;
    };
    // Kept sorted by phase, stable within a phase. Sorting once at setup keeps the
    // per-frame walk a straight line through contiguous memory.
    std::vector<Entry> systems_;
    bool               sealed_ = false;
};

} // namespace rgv::ecs
