#include "LiveBootstrap.h"

#include "rgv/ecs/Resources.h"
#include "rgv/live/LiveSource.h"

#include <cstdio>
#include <memory>

namespace rgv::app {
namespace {

// Owns the attached source. A resource for the same reason the fixture one is: swapping
// it must not leave anything holding a dangling pointer.
struct LiveOwner {
    std::unique_ptr<live::LiveSource> source;
};

} // namespace

bool attach_live_source(ecs::World& world, const std::string& provider,
                        const std::string& root) {
    auto& owner = world.add_resource<LiveOwner>();
    try {
        owner.source = std::make_unique<live::LiveSource>(
            std::vector<std::string>{live::resolve_provider(provider), "--root", root});
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "could not attach live source: %s\n"
                     "  provider: %s\n"
                     "  root:     %s\n",
                     ex.what(), provider.c_str(), root.c_str());
        return false;
    }

    auto& handle = world.resource<ecs::SourceHandle>();
    handle.source = owner.source.get();
    // No fixtures pointer: there is no timeline to scrub, and the UI keys off exactly
    // that rather than asking what kind of source it has.
    handle.fixtures = nullptr;

    for (const auto& line : owner.source->log()) std::fprintf(stderr, "provider: %s\n", line.c_str());
    return true;
}

} // namespace rgv::app
