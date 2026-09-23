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

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;
using rgv::platform::openable_path;
using rgv::platform::opener_argv;
using rgv::platform::open_with;

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


// -- which program gets handed the path ---------------------------------------
//
// `xdg-open` is the right default and the wrong answer often enough to need an escape
// hatch: on a desktop whose handler for `text/x-c++src` is a `Terminal=true` entry --
// nvim, helix, emacs -nw -- the launcher tries to run an editor with no terminal to run
// it in, and the file silently never opens. `open_command` is how a user says what to
// run instead.
//
// Split on spaces and exec'd directly. No shell anywhere, same as the launcher path, so
// nothing in a filename can be read as one.

TEST(no_open_command_means_the_desktop_decides) {
    const auto argv = opener_argv("", "/repo/src/a.cpp");
    CHECK_EQ(argv.size(), 2u);
    CHECK(argv[0] == "xdg-open" || argv[0] == "open");
    CHECK_EQ(argv[1], std::string("/repo/src/a.cpp"));
}

TEST(a_placeholder_says_where_the_path_goes) {
    const auto argv = opener_argv("kitty -e nvim {} +1", "/repo/a b.cpp");
    CHECK_EQ(argv.size(), 5u);
    CHECK_EQ(argv[0], std::string("kitty"));
    CHECK_EQ(argv[1], std::string("-e"));
    CHECK_EQ(argv[2], std::string("nvim"));
    CHECK_EQ(argv[3], std::string("/repo/a b.cpp"));   // one argv slot, spaces and all
    CHECK_EQ(argv[4], std::string("+1"));
}

TEST(without_a_placeholder_the_path_is_appended) {
    const auto argv = opener_argv("gedit", "/repo/a.cpp");
    CHECK_EQ(argv.size(), 2u);
    CHECK_EQ(argv[0], std::string("gedit"));
    CHECK_EQ(argv[1], std::string("/repo/a.cpp"));
}

TEST(extra_spacing_in_the_command_is_not_an_empty_argument) {
    const auto argv = opener_argv("  code   -g   {}  ", "/repo/a.cpp");
    CHECK_EQ(argv.size(), 3u);
    CHECK_EQ(argv[0], std::string("code"));
    CHECK_EQ(argv[1], std::string("-g"));
    CHECK_EQ(argv[2], std::string("/repo/a.cpp"));
}

// Launch failure has to be reportable, or "nothing happened" is indistinguishable from
// "it worked". This used to return true unconditionally: the launcher is reparented so
// its exit status is gone by design, and the error branch in CommandSystem could never
// fire. What IS knowable is whether the program started at all.
TEST(a_launcher_that_does_not_exist_is_reported) {
    Tree        t("open-launch-missing");
    std::string err;
    const bool  ok =
        open_with((t.root / "README.md").string(), "rgv-no-such-program-hopefully", &err);
    CHECK(!ok);
    CHECK(!err.empty());
}

TEST(a_launcher_that_starts_is_not_reported_as_a_failure) {
    Tree        t("open-launch-ok");
    std::string err;
    // `/usr/bin/true` ignores its argument and exits 0 -- enough to prove the path runs
    // without opening a window or an editor in a test. Absolute, because a test should
    // not depend on what PATH happens to hold.
    CHECK(open_with((t.root / "README.md").string(), "/usr/bin/true {}", &err));
    CHECK(err.empty());
}
