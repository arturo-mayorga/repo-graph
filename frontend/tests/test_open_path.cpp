// Which path the desktop's launcher is allowed to be handed.
//
// A path arrives over the wire from a provider, which is a separate process speaking a
// wire format, so it is input. Handing a path to `xdg-open` is handing the desktop a
// program to run, and the only paths this feature needs are the ones under the
// repository the user asked to watch.

#include "TestMain.h"

#include "rgv/platform/OpenPath.h"

#include <filesystem>
#include <fstream>

#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
using rgv::platform::openable_path;

namespace {

struct Tree {
    fs::path root;
    explicit Tree(const char* name) : root(fs::path(RGV_TEST_TMP) / name) {
        fs::remove_all(root);
        fs::create_directories(root / "src");
        std::ofstream(root / "src" / "app.py") << "x = 1\n";
        std::ofstream(root / "README.md") << "hi\n";
        fs::create_directories(root.parent_path() / "outside");
        std::ofstream(root.parent_path() / "outside" / "secret.txt") << "no\n";
    }
};

} // namespace

TEST(a_file_inside_the_repository_resolves_to_its_absolute_path) {
    Tree t("open-inside");
    const std::string got = openable_path(t.root.string(), "src/app.py");
    CHECK(!got.empty());
    CHECK_EQ(fs::path(got).filename().string(), std::string("app.py"));
    CHECK(fs::exists(got));
}

TEST(a_directory_resolves_too_so_it_can_be_handed_to_a_file_manager) {
    Tree t("open-dir");
    CHECK(!openable_path(t.root.string(), "src").empty());
}

// The bound. A provider is not this program, and a path that climbs out of the tree is
// not one the user asked to look at.
TEST(a_path_that_leaves_the_repository_is_refused) {
    Tree t("open-escape");
    CHECK(openable_path(t.root.string(), "../outside/secret.txt").empty());
    CHECK(openable_path(t.root.string(), "src/../../outside/secret.txt").empty());
}

TEST(a_symlink_pointing_out_of_the_repository_is_refused) {
    Tree            t("open-symlink");
    std::error_code ec;
    fs::create_symlink(t.root.parent_path() / "outside" / "secret.txt", t.root / "link.txt", ec);
    if (ec) return;   // a filesystem without symlinks has nothing to prove here
    CHECK(openable_path(t.root.string(), "link.txt").empty());
}

TEST(a_path_that_does_not_exist_is_refused) {
    Tree t("open-missing");
    CHECK(openable_path(t.root.string(), "src/gone.py").empty());
}

TEST(an_empty_root_or_path_is_refused) {
    Tree t("open-empty");
    CHECK(openable_path("", "src/app.py").empty());
    CHECK(openable_path(t.root.string(), "").empty());
}

// The launch itself, against a stand-in launcher on PATH. Nothing else proves the
// fork/exec actually runs, or that a path with a space in it survives as one argument
// -- there is no shell in the path, and this is what says so.
TEST(the_launcher_is_started_with_the_path_as_a_single_argument) {
    Tree t("open-spawn");
    const fs::path dir = t.root / "a dir with spaces";
    fs::create_directories(dir);
    std::ofstream(dir / "it's here.txt") << "x\n";

    const fs::path bin    = fs::path(RGV_TEST_TMP) / "open-spawn-bin";
    const fs::path marker = fs::path(RGV_TEST_TMP) / "open-spawn.out";
    fs::create_directories(bin);
    fs::remove(marker);
    for (const char* name : {"xdg-open", "open"}) {
        std::ofstream f(bin / name);
        f << "#!/bin/sh\nprintf '%s' \"$1\" > '" << marker.string() << "'\n";
        f.close();
        fs::permissions(bin / name, fs::perms::owner_all);
    }

    const char* had = std::getenv("PATH");
    const std::string old_path = had ? had : "";
    ::setenv("PATH", (bin.string() + ":" + old_path).c_str(), 1);

    const std::string target = openable_path(t.root.string(), "a dir with spaces/it's here.txt");
    CHECK(!target.empty());
    std::string err;
    const bool  started = rgv::platform::open_in_default_app(target, &err);
    for (int i = 0; i < 300 && !fs::exists(marker); ++i) ::usleep(10000);
    ::setenv("PATH", old_path.c_str(), 1);

    CHECK(started);
    CHECK(fs::exists(marker));
    std::ifstream in(marker);
    std::string   got;
    std::getline(in, got);
    CHECK_EQ(got, target);
}

