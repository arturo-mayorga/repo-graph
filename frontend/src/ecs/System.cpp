#include "rgv/ecs/System.h"

#include <algorithm>
#include <cassert>

namespace rgv::ecs {

std::string_view to_string(Phase p) {
    switch (p) {
        case Phase::Input:    return "Input";
        case Phase::Ingest:   return "Ingest";
        case Phase::Sync:     return "Sync";
        case Phase::Simulate: return "Simulate";
        case Phase::Render:   return "Render";
        case Phase::Present:  return "Present";
    }
    return "?";
}

Schedule& Schedule::add(Phase phase, std::unique_ptr<System> system) {
    assert(!sealed_ && "systems must be attached before setup()");
    assert(system && "null system");
    systems_.push_back(Entry{phase, std::move(system)});
    return *this;
}

void Schedule::setup(World& world) {
    // Stable sort: phases order the frame, insertion order orders within a phase.
    std::stable_sort(systems_.begin(), systems_.end(), [](const Entry& a, const Entry& b) {
        return static_cast<int>(a.phase) < static_cast<int>(b.phase);
    });
    sealed_ = true;

    for (auto& e : systems_) e.system->setup(world);
}

void Schedule::run(World& world, const FrameContext& frame) {
    for (auto& e : systems_) e.system->run(world, frame);
}

void Schedule::teardown(World& world) {
    // Reverse order, so a system can rely on what it was set up alongside still
    // existing while it tears down.
    for (auto it = systems_.rbegin(); it != systems_.rend(); ++it) {
        (*it).system->teardown(world);
    }
}

std::vector<std::string> Schedule::listing() const {
    std::vector<std::string> out;
    out.reserve(systems_.size());
    for (const auto& e : systems_) {
        out.push_back(std::string(to_string(e.phase)) + "/" + std::string(e.system->name()));
    }
    return out;
}

} // namespace rgv::ecs
