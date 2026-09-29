// The C++ reader: includes out of source, files out of includes, and the build targets
// that stand in for packages.
//
// Two things make this different from the Python and TypeScript readers. Resolution has
// no packaging to read -- a quoted include is looked up beside the including file and
// then against whatever directories a repository conventionally puts headers in -- and
// the architectural unit is not a directory. A build target lists sources from several
// directories and one file can be listed in two of them, so ownership is a relation and
// the containment parent is only its first claim.

#include "TestMain.h"

#include "CppImports.h"

#include <map>
#include <set>
#include <string>

using namespace rgv;

namespace {

const watch::CppInclude* inc_for(const std::vector<watch::CppInclude>& is, const std::string& p) {
    for (const auto& i : is) {
        if (i.path == p) return &i;
    }
    return nullptr;
}

struct Repo {
    std::map<std::string, bool> entries;   // rel -> is_directory
    std::set<std::string>       files;

    void file(const std::string& rel) {
        files.insert(rel);
        entries[rel] = false;
        std::string dir = rel;
        for (;;) {
            const auto slash = dir.rfind('/');
            if (slash == std::string::npos) break;
            dir = dir.substr(0, slash);
            entries[dir] = true;
        }
    }
    std::vector<std::string> roots() const { return watch::cpp_source_roots(entries); }
    std::string resolve(const std::string& from, const std::string& path, bool angled,
                        bool* ambiguous = nullptr) const {
        return watch::resolve_cpp_include(from, watch::CppInclude{path, angled, 1, ""}, files,
                                          roots(), ambiguous);
    }
};

} // namespace

// -- parsing ------------------------------------------------------------------

TEST(both_include_forms_are_read_with_their_line_and_statement) {
    const auto is = watch::parse_cpp_includes(
        "#include \"local.h\"\n"
        "#include <vector>\n"
        "#include <rgv/model/GraphStore.h>\n");
    CHECK_EQ(is.size(), 3u);
    CHECK(!inc_for(is, "local.h")->angled);
    CHECK(inc_for(is, "vector")->angled);
    CHECK_EQ(inc_for(is, "vector")->line, 2);
    CHECK_EQ(inc_for(is, "rgv/model/GraphStore.h")->snippet,
             std::string("#include <rgv/model/GraphStore.h>"));
}

// The directive is `#` then `include`, and a compiler does not care how much space is
// between them or in front of them.
TEST(spacing_around_the_directive_does_not_hide_it) {
    const auto is = watch::parse_cpp_includes("  #   include   \"a.h\"\n");
    CHECK_EQ(is.size(), 1u);
    CHECK_EQ(is[0].path, std::string("a.h"));
}

TEST(an_include_inside_a_comment_is_not_an_include) {
    const auto is = watch::parse_cpp_includes(
        "// #include \"commented.h\"\n"
        "/* #include \"blocked.h\"\n"
        "   #include \"still-blocked.h\" */\n"
        "#include \"real.h\"   // #include \"trailing.h\"\n");
    CHECK_EQ(is.size(), 1u);
    CHECK_EQ(is[0].path, std::string("real.h"));
    CHECK_EQ(is[0].line, 4);
}

// A test that writes a C++ fixture into a temp file is full of text that looks like
// source. It is a string, and a string is not a directive.
TEST(an_include_inside_a_string_literal_is_not_an_include) {
    const auto is = watch::parse_cpp_includes(
        "const char* src = \"#include \\\"fake.h\\\"\\n\";\n"
        "const char* raw = R\"(\n"
        "#include \"also-fake.h\"\n"
        ")\";\n"
        "#include \"real.h\"\n");
    CHECK_EQ(is.size(), 1u);
    CHECK_EQ(is[0].path, std::string("real.h"));
}

TEST(a_directive_continued_over_lines_is_still_one_include) {
    const auto is = watch::parse_cpp_includes("#include \\\n    \"wrapped.h\"\n");
    CHECK_EQ(is.size(), 1u);
    CHECK_EQ(is[0].path, std::string("wrapped.h"));
    CHECK_EQ(is[0].line, 1);
}

TEST(an_include_of_a_macro_is_not_guessed_at) {
    const auto is = watch::parse_cpp_includes("#define H \"a.h\"\n#include H\n");
    CHECK_EQ(is.size(), 0u);
}

// -- resolution ---------------------------------------------------------------

TEST(a_quoted_include_finds_the_header_beside_it) {
    Repo r;
    r.file("provider/watch/Impact.cpp");
    r.file("provider/watch/Impact.h");
    CHECK_EQ(r.resolve("provider/watch/Impact.cpp", "Impact.h", false),
             std::string("provider/watch/Impact.h"));
}

// A header and its implementation are two nodes, and the edge between them is the most
// common one a C++ repository has. Nothing special-cases it away.
TEST(a_source_file_depends_on_its_own_header) {
    Repo r;
    r.file("src/Foo.cpp");
    r.file("include/Foo.h");
    const auto found = watch::resolve_cpp_includes(
        "src/Foo.cpp", watch::parse_cpp_includes("#include \"Foo.h\"\n"), r.files, r.roots());
    CHECK_EQ(found.size(), 1u);
    CHECK_EQ(found[0].to, std::string("include/Foo.h"));
    CHECK(!found[0].ambiguous);
}

TEST(an_angled_include_resolves_against_the_include_directories) {
    Repo r;
    r.file("frontend/src/model/GraphStore.cpp");
    r.file("frontend/include/rgv/model/GraphStore.h");
    CHECK_EQ(r.resolve("frontend/src/model/GraphStore.cpp", "rgv/model/GraphStore.h", true),
             std::string("frontend/include/rgv/model/GraphStore.h"));
    // The quoted form reaches the same file: beside it first, then the same roots.
    CHECK_EQ(r.resolve("frontend/src/model/GraphStore.cpp", "rgv/model/GraphStore.h", false),
             std::string("frontend/include/rgv/model/GraphStore.h"));
}

TEST(an_include_of_something_outside_the_repository_produces_nothing) {
    Repo r;
    r.file("src/a.cpp");
    CHECK_EQ(r.resolve("src/a.cpp", "vector", true), std::string(""));
    CHECK_EQ(r.resolve("src/a.cpp", "entt/entt.hpp", true), std::string(""));
    CHECK_EQ(r.resolve("src/a.cpp", "nowhere.h", false), std::string(""));

    const auto found = watch::resolve_cpp_includes(
        "src/a.cpp",
        watch::parse_cpp_includes("#include <vector>\n#include <entt/entt.hpp>\n"), r.files,
        r.roots());
    CHECK_EQ(found.size(), 0u);
}

// Two roots satisfying one include is a guess, and a guess is `heuristic` -- which the
// traversal skips by default. The pick is the first root so the stream is deterministic.
TEST(an_include_two_roots_could_satisfy_is_ambiguous) {
    Repo r;
    r.file("app/main.cpp");
    r.file("include/shared/util.h");
    r.file("src/shared/util.h");
    bool ambiguous = false;
    const std::string hit = r.resolve("app/main.cpp", "shared/util.h", true, &ambiguous);
    CHECK(ambiguous);
    CHECK_EQ(hit, std::string("include/shared/util.h"));   // the first root that has it
    CHECK_EQ(r.resolve("app/main.cpp", "shared/util.h", true), hit);   // and it is stable
}

// The file's own directory is where the standard says a quoted include looks first, so
// finding it there settles the question -- a same-named header under some root is not a
// second candidate.
TEST(a_quoted_include_found_beside_the_file_is_not_ambiguous) {
    Repo r;
    r.file("src/shared/util.cpp");
    r.file("src/shared/util.h");
    r.file("include/util.h");
    bool ambiguous = true;
    CHECK_EQ(r.resolve("src/shared/util.cpp", "util.h", false, &ambiguous),
             std::string("src/shared/util.h"));
    CHECK(!ambiguous);
}

TEST(a_relative_include_walks_out_of_its_directory) {
    Repo r;
    r.file("src/ui/panel.cpp");
    r.file("src/core/theme.h");
    CHECK_EQ(r.resolve("src/ui/panel.cpp", "../core/theme.h", false),
             std::string("src/core/theme.h"));
    CHECK_EQ(r.resolve("src/ui/panel.cpp", "../../../escape.h", false), std::string(""));
}

TEST(one_edge_per_target_however_many_times_it_is_included) {
    Repo r;
    r.file("src/a.cpp");
    r.file("src/b.h");
    const auto found = watch::resolve_cpp_includes(
        "src/a.cpp",
        watch::parse_cpp_includes("#include \"b.h\"\n#include <vector>\n#include \"b.h\"\n"),
        r.files, r.roots());
    CHECK_EQ(found.size(), 1u);
    CHECK_EQ(found[0].line, 1);
}

// -- build targets ------------------------------------------------------------

TEST(add_executable_and_add_library_are_the_targets) {
    const auto ts = watch::parse_cmake_targets(
        "add_library(rgv_core STATIC\n"
        "  src/model/GraphStore.cpp\n"
        "  src/sim/ImpactSim.cpp\n"
        ")\n"
        "add_executable(rgv app/main.cpp)\n",
        "frontend");
    CHECK_EQ(ts.size(), 2u);
    CHECK_EQ(ts[0].name, std::string("rgv_core"));
    CHECK_EQ(ts[0].kind, std::string("library"));
    CHECK_EQ(ts[0].line, 1);
    CHECK_EQ(ts[0].sources.size(), 2u);
    CHECK_EQ(ts[0].sources[0], std::string("frontend/src/model/GraphStore.cpp"));
    CHECK_EQ(ts[1].name, std::string("rgv"));
    CHECK_EQ(ts[1].kind, std::string("executable"));
    CHECK_EQ(ts[1].sources[0], std::string("frontend/app/main.cpp"));
}

// The one variable a scanner can expand without a CMake interpreter is the directory
// the file is in. Anything else is dropped: a source list is evidence, not a guess.
TEST(the_current_source_dir_is_expanded_and_other_variables_are_dropped) {
    const auto ts = watch::parse_cmake_targets(
        "add_executable(rgv-watch\n"
        "  ${CMAKE_CURRENT_SOURCE_DIR}/../provider/watch/main.cpp\n"
        "  ${GENERATED_SOURCES}\n"
        "  $<$<BOOL:${WIN32}>:win.cpp>\n"
        "  tools/rgv_replay.cpp)\n",
        "frontend");
    CHECK_EQ(ts.size(), 1u);
    CHECK_EQ(ts[0].sources.size(), 2u);
    CHECK_EQ(ts[0].sources[0], std::string("provider/watch/main.cpp"));
    CHECK_EQ(ts[0].sources[1], std::string("frontend/tools/rgv_replay.cpp"));
}

TEST(target_keywords_and_comments_are_not_sources) {
    const auto ts = watch::parse_cmake_targets(
        "add_executable(app WIN32 MACOSX_BUNDLE\n"
        "  # the real one\n"
        "  main.cpp   # trailing\n"
        "  EXCLUDE_FROM_ALL)\n"
        "add_library(iface INTERFACE)\n"
        "add_library(alias ALIAS iface)\n",
        "");
    CHECK_EQ(ts.size(), 2u);   // the alias is another name for a target, not a target
    CHECK_EQ(ts[0].sources.size(), 1u);
    CHECK_EQ(ts[0].sources[0], std::string("main.cpp"));
    CHECK_EQ(ts[1].name, std::string("iface"));
    CHECK_EQ(ts[1].sources.size(), 0u);
}

// -- ownership ----------------------------------------------------------------

namespace {

// The shape a real C++ repository has: a library of sources with its headers in an
// `include/` tree no build file mentions, an application that links it, and a test
// binary that compiles one of the library's files into itself.
struct Tree {
    Repo                                            repo;
    std::vector<watch::CppTarget>                   targets;
    std::map<std::string, std::vector<std::string>> includes;

    void file(const std::string& rel) { repo.file(rel); }
    void target(const std::string& name, const std::string& kind,
                std::vector<std::string> sources) {
        targets.push_back(watch::CppTarget{name, kind, "CMakeLists.txt", 1, "", std::move(sources)});
    }
    void include(const std::string& from, const std::string& to) { includes[from].push_back(to); }
    watch::CppOwnership own() const {
        return watch::cpp_ownership(targets, repo.entries, includes);
    }
};

Tree sample() {
    Tree t;
    t.file("lib/src/core.cpp");
    t.file("lib/src/util.cpp");
    t.file("lib/include/lib/core.h");
    t.file("lib/include/lib/util.h");
    t.file("app/main.cpp");
    t.file("CMakeLists.txt");
    t.target("core", "library", {"lib/src/core.cpp", "lib/src/util.cpp"});
    t.target("app", "executable", {"app/main.cpp"});
    t.include("lib/src/core.cpp", "lib/include/lib/core.h");
    t.include("lib/src/util.cpp", "lib/include/lib/util.h");
    t.include("lib/include/lib/core.h", "lib/include/lib/util.h");
    t.include("app/main.cpp", "lib/include/lib/core.h");
    return t;
}

} // namespace

// A .cpp never includes another .cpp, so a target's headers are the only thing that can
// carry an edge from one target to another. Reaching them is what makes the
// architecture view something other than boxes.
TEST(a_target_owns_the_headers_its_sources_reach) {
    const auto own = sample().own();
    CHECK_EQ(own.owners.at("lib/src/core.cpp")[0], std::string("core"));
    CHECK_EQ(own.owners.at("lib/include/lib/core.h")[0], std::string("core"));
    // Reached only through another header, and still the library's.
    CHECK_EQ(own.owners.at("lib/include/lib/util.h")[0], std::string("core"));
    CHECK_EQ(own.owners.at("app/main.cpp")[0], std::string("app"));
}

// `app` reaches `core.h` too. That is a second claim on one file, and the contract
// wants one parent and an `owns` edge for the rest, never two parents.
TEST(a_header_several_targets_reach_has_one_parent_and_keeps_the_other_claims) {
    const auto own = sample().own();
    const auto& claims = own.owners.at("lib/include/lib/core.h");
    CHECK_EQ(claims.size(), 2u);
    CHECK_EQ(claims[0], std::string("core"));   // the target whose own sources sit deepest
    CHECK_EQ(claims[1], std::string("app"));
    // Reaching a header is not a claim on it: only a source list is.
    CHECK(!own.listed.count("lib/include/lib/core.h"));
}

// A header every target includes belongs to the library among them. A one-file tool
// sitting deeper in the tree is not a better claim than the library whose headers they
// are: let it win and every dependency the architecture view draws points backwards.
TEST(a_header_a_library_and_a_tool_share_belongs_to_the_library) {
    Tree t;
    t.file("src/core.cpp");
    t.file("include/core.h");
    t.file("tools/deep/down/dump.cpp");
    t.target("core", "library", {"src/core.cpp"});
    t.target("dump", "executable", {"tools/deep/down/dump.cpp"});
    t.include("src/core.cpp", "include/core.h");
    t.include("tools/deep/down/dump.cpp", "include/core.h");
    const auto own = t.own();
    CHECK_EQ(own.owners.at("include/core.h")[0], std::string("core"));
}

// A file listed in two targets belongs to the one that is most specific about where its
// code lives -- the provider's own binary, not the test binary that also compiles it.
TEST(a_source_listed_in_two_targets_belongs_to_the_more_specific_one) {
    Tree t;
    t.file("provider/watch/main.cpp");
    t.file("provider/watch/Packages.cpp");
    t.file("frontend/tests/test_main.cpp");
    t.target("rgv-watch", "executable", {"provider/watch/main.cpp", "provider/watch/Packages.cpp"});
    t.target("rgv-tests", "executable", {"frontend/tests/test_main.cpp", "provider/watch/Packages.cpp"});
    const auto own = t.own();
    CHECK_EQ(own.owners.at("provider/watch/Packages.cpp")[0], std::string("rgv-watch"));
    CHECK_EQ(own.owners.at("provider/watch/Packages.cpp").size(), 2u);
    // Both build files name it, which is what an `owns` edge is for.
    CHECK_EQ(own.listed.at("provider/watch/Packages.cpp").size(), 2u);
    CHECK_EQ(own.listed.at("provider/watch/Packages.cpp")[1], std::string("rgv-tests"));
    // Listing beats reaching: a target that names a file outranks one that includes it.
    CHECK_EQ(own.owners.at("frontend/tests/test_main.cpp")[0], std::string("rgv-tests"));
}

// The node has to sit somewhere, and a target whose code is one directory sits on it.
TEST(a_target_sits_on_the_deepest_directory_that_holds_it) {
    const auto own = sample().own();
    CHECK_EQ(own.home.at("core"), std::string("lib"));   // src/ and include/ both
    CHECK_EQ(own.home.at("app"), std::string("app"));
}

// Containment is the only thing the architecture view can fold an import along, so the
// files have to hang off their target. Moving a whole directory when every owned file
// in it belongs to one target is what keeps the filesystem view's tree recognisable.
TEST(a_directory_whose_files_all_belong_to_one_target_moves_as_a_whole) {
    const auto own = sample().own();
    CHECK_EQ(own.parent.at("lib/src"), std::string("core"));
    CHECK_EQ(own.parent.at("lib/include"), std::string("core"));
    // The files inside it keep their directory: the directory already moved.
    CHECK(!own.parent.count("lib/src/core.cpp"));
    // `app/main.cpp` sits directly in its target's home, so the file moves itself --
    // moving `app/` onto a target that lives at `app/` would be a loop.
    CHECK_EQ(own.parent.at("app/main.cpp"), std::string("app"));
    CHECK(!own.parent.count("app"));
}

TEST(a_directory_split_between_targets_moves_its_files_one_at_a_time) {
    Tree t;
    t.file("src/ui/panels.cpp");
    t.file("src/ui/theme.cpp");
    t.file("src/core/store.cpp");
    t.file("app/main.cpp");
    t.target("core", "library", {"src/core/store.cpp", "src/ui/theme.cpp"});
    t.target("app", "executable", {"app/main.cpp", "src/ui/panels.cpp"});
    const auto own = t.own();
    CHECK(!own.parent.count("src/ui"));
    CHECK_EQ(own.parent.at("src/ui/theme.cpp"), std::string("core"));
    CHECK_EQ(own.parent.at("src/ui/panels.cpp"), std::string("app"));
    CHECK_EQ(own.parent.at("src/core"), std::string("core"));
}

// A target the build declares but whose sources are not in this repository owns nothing
// and is not a node. An interface library is compile flags, not a unit of architecture.
TEST(a_target_with_nothing_of_ours_in_it_is_not_a_node) {
    Tree t;
    t.file("src/a.cpp");
    t.target("real", "library", {"src/a.cpp"});
    t.target("warnings", "library", {});
    t.target("vendored", "library", {"third_party/absent/x.cpp"});
    const auto own = t.own();
    CHECK_EQ(own.names.size(), 1u);
    CHECK_EQ(own.names[0], std::string("real"));
}
