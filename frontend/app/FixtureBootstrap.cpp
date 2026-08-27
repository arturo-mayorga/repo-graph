#include "FixtureBootstrap.h"

#include "rgv/ecs/Resources.h"
#include "rgv/fixture/FixtureSource.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace rgv::app {
namespace fs = std::filesystem;
namespace {

// Owns whatever IGraphSource is attached. A resource so that swapping fixtures can
// replace it without anything else holding a dangling pointer.
struct SourceOwner {
    std::unique_ptr<fixture::FixtureSource> source;
};

std::vector<std::string> discover(const std::string& root) {
    std::vector<std::string> out;
    std::error_code          ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (e.is_directory() && fs::exists(e.path() / "snapshot.json")) {
            out.push_back(e.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Installed into FixtureLibrary so CommandSystem can swap fixtures without knowing
// anything about them.
bool load(ecs::World& world, int index) {
    auto& library = world.resource<ecs::FixtureLibrary>();
    if (index < 0 || index >= static_cast<int>(library.dirs.size())) return false;
    try {
        auto  set   = fixture::FixtureSet::load(library.dirs[static_cast<std::size_t>(index)]);
        auto& owner = world.resource<SourceOwner>();
        owner.source = std::make_unique<fixture::FixtureSource>(std::move(set), 0);

        auto& handle    = world.resource<ecs::SourceHandle>();
        handle.source   = owner.source.get();
        handle.fixtures = owner.source.get();
        library.current = index;

        world.resource<ecs::Selection>() = ecs::Selection{};
        return true;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "fixture load failed: %s\n", ex.what());
        return false;
    }
}

} // namespace

bool attach_fixture_source(ecs::World& world, const std::string& root,
                           const std::string& preferred, int scenario) {
    world.add_resource<SourceOwner>();
    auto& library = world.resource<ecs::FixtureLibrary>();
    library.dirs  = discover(root);
    library.load  = load;

    if (library.dirs.empty()) {
        std::fprintf(stderr,
                     "no fixture sets found under %s\n"
                     "expected subdirectories each containing snapshot.json\n",
                     root.c_str());
        return false;
    }
    for (const auto& d : library.dirs) library.names.push_back(fs::path(d).filename().string());

    int initial = 0;
    if (!preferred.empty()) {
        for (std::size_t i = 0; i < library.names.size(); ++i) {
            if (library.names[i] == preferred) initial = static_cast<int>(i);
        }
    } else {
        // Smallest first: the readable fixture teaches the interaction, and the scale
        // probe is one dropdown away.
        std::uintmax_t smallest = 0;
        for (std::size_t i = 0; i < library.dirs.size(); ++i) {
            std::error_code ec;
            const auto sz = fs::file_size(fs::path(library.dirs[i]) / "snapshot.json", ec);
            if (ec) continue;
            if (smallest == 0 || sz < smallest) { smallest = sz; initial = static_cast<int>(i); }
        }
    }

    if (!load(world, initial)) return false;
    if (scenario > 0) {
        world.resource<SourceOwner>().source->select_scenario(static_cast<std::size_t>(scenario));
    }
    return true;
}

} // namespace rgv::app
