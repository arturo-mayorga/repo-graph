// What the walk descends into.
//
// The provider's first answer about a repository is which files are in it, and it is
// the answer every other adapter is built on: a phantom directory produces phantom
// packages, phantom imports and a phantom architecture. Pointing the viewer at this
// repository used to report 24 packages, all 24 of them temp repositories the test
// suite had written under `build-headless/` -- a directory the hardcoded skip list did
// not happen to name.
//
// So the rules are asserted here rather than discovered at a demo.

#include "TestMain.h"

#include "Ignore.h"

#include <string>

using namespace rgv;

TEST(the_expensive_directories_are_skipped_wherever_they_sit) {
    const watch::Ignores ig;
    CHECK(ig.skips("node_modules", true));
    CHECK(ig.skips("web/app/node_modules", true));
    CHECK(ig.skips("build", true));
    CHECK(ig.skips("target", true));
    CHECK(ig.skips(".git", true));
    CHECK(ig.skips("src/.venv", true));
}

// The reason the skip list is not a prefix match. `builder/` and `buildings/` are
// source directories with an unlucky spelling, and a provider that swallowed them
// would be hiding code -- a far worse failure than showing a build tree.
TEST(a_directory_that_merely_starts_with_build_is_still_walked) {
    const watch::Ignores ig;
    CHECK(!ig.skips("builder", true));
    CHECK(!ig.skips("buildings", true));
    CHECK(!ig.skips("src/build_rules", true));
    // And, with nothing ignoring it, neither is this one: unlisted means content.
    CHECK(!ig.skips("build-headless", true));
}

// The repository says which of its directories are derived. This one says `build-*/`,
// which is exactly the case the name list missed.
TEST(the_repositorys_own_gitignore_prunes_its_build_trees) {
    watch::Ignores ig;
    ig.add_gitignore("build/\nbuild-*/\n.cache/\ncompile_commands.json\n");

    CHECK(ig.skips("build-headless", true));
    CHECK(ig.skips("build-clang-asan", true));
    CHECK(ig.skips("compile_commands.json", false));
    // Still not these. A pattern is a pattern, not a prefix.
    CHECK(!ig.skips("builder", true));
    CHECK(!ig.skips("buildings", true));
    // `build-*/` has a trailing slash, so it names directories only.
    CHECK(!ig.skips("build-notes.md", false));
}

// A pattern with a slash in it is anchored to the repository root, which is what keeps
// `doc/frotz/` from taking `a/doc/frotz/` with it.
TEST(a_pattern_with_a_slash_is_anchored_to_the_root) {
    watch::Ignores ig;
    ig.add_gitignore("/out\ndoc/frotz/\n");
    CHECK(ig.skips("out", true));
    CHECK(!ig.skips("sub/out", true));
    CHECK(ig.skips("doc/frotz", true));
    CHECK(!ig.skips("a/doc/frotz", true));
}

TEST(a_bare_pattern_matches_a_name_at_any_depth_and_a_star_stops_at_a_slash) {
    watch::Ignores ig;
    ig.add_gitignore("*.o\nsrc/*.tmp\n**/generated\n");
    CHECK(ig.skips("a.o", false));
    CHECK(ig.skips("lib/src/a.o", false));
    CHECK(ig.skips("src/x.tmp", false));
    CHECK(!ig.skips("src/deep/x.tmp", false));   // `*` does not cross a separator
    CHECK(ig.skips("a/b/generated", true));      // a leading `**/` is just unanchored
}

TEST(comments_and_blank_lines_are_not_patterns) {
    watch::Ignores ig;
    ig.add_gitignore("# frontend\n\n   \nfrontend_out\n");
    CHECK(!ig.skips("frontend", true));
    CHECK(ig.skips("frontend_out", true));
}

// Last match wins, which is the only reason a negation means anything.
TEST(a_later_negation_wins_over_an_earlier_pattern) {
    watch::Ignores ig;
    ig.add_gitignore("*.log\n!keep.log\n");
    CHECK(ig.skips("run.log", false));
    CHECK(!ig.skips("keep.log", false));
}

// But not over the built-in list. Those directories are skipped because watching them
// costs more than they are worth, not because the repository does not track them, and
// a repository that commits its `node_modules` has not made it interesting.
TEST(a_negation_cannot_re_admit_a_built_in_skip) {
    watch::Ignores ig;
    ig.add_gitignore("!node_modules\n!build\n");
    CHECK(ig.skips("node_modules", true));
    CHECK(ig.skips("build", true));
}

// The supported subset is a subset on purpose, and it fails in the direction that shows
// too much rather than too little: a pattern this matcher cannot read is dropped, so
// the walk descends where git would not. Showing a derived file is noise; hiding a
// source file is a lie about the repository.
TEST(a_pattern_outside_the_supported_subset_is_dropped_rather_than_guessed) {
    watch::Ignores ig;
    ig.add_gitignore("[abc].txt\nsrc/**/gen\nwith\\ space\n");
    CHECK(!ig.skips("a.txt", false));
    CHECK(!ig.skips("src/x/gen", true));
    CHECK(!ig.skips("with space", false));
}

// Dotfiles were already out of the walk's way before there was a gitignore to read;
// the rule lives here now so the walk asks exactly one question.
TEST(dot_names_stay_out_of_the_way) {
    const watch::Ignores ig;
    CHECK(ig.skips(".mypy_cache", true));
    CHECK(ig.skips("src/.env", false));
    CHECK(!ig.skips("src/a.env", false));
}
