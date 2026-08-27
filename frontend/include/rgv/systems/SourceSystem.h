// Ingest: pull whatever the attached source has produced into the graph store.
//
// This is the seam the whole design turns on. Today the source replays authored
// fixtures; tomorrow it is a filesystem watcher or a socket. Neither this system nor
// anything downstream changes -- only which IGraphSource was installed.
#pragma once

#include "rgv/contract/IGraphSource.h"
#include "rgv/ecs/System.h"

namespace rgv::systems {

class SourceSystem final : public ecs::System {
public:
    std::string_view name() const override { return "SourceSystem"; }
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    // The source this system has already seeded the store from. Comparing pointers is
    // how "a different source is attached" is detected without asking what kind it is.
    const IGraphSource* seeded_ = nullptr;
};

} // namespace rgv::systems
