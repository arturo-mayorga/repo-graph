// C++ include extraction, and the build targets that stand in for packages.
//
// The same two separable steps as the Python reader, for the same reason. Parsing turns
// source text into the `#include` directives it contains and is a property of one file.
// Resolution turns a directive into a repo-relative file and depends on the whole tree,
// so a file's edges can change without the file changing at all -- a header appears
// beside it, an include directory is added.
//
// A line scanner, not a preprocessor. An include is a directive on a line of its own,
// and the cases a scanner cannot see -- `#include MACRO`, a path assembled by the build
// -- are exactly the ones a dependency graph should not claim to know. What it must not
// do is invent an edge out of a comment or a string literal, so both are tracked, raw
// strings included: a test that writes a C++ fixture into a temp file is full of text
// that looks like source and is not.
//
// An include that does not resolve to a file in this repository produces nothing.
// `<vector>` and `<entt/entt.hpp>` are real dependencies, but not on anything here, and
// an edge to a node that does not exist is worse than no edge (contract 6.4).
#pragma once

#include "FileImport.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace rgv::watch {

// One `#include`, as written, with where it was written.
struct CppInclude {
    std::string path;            // between the delimiters: "rgv/model/GraphStore.h"
    bool        angled = false;  // <> rather than ""
    int         line = 0;        // 1-based
    std::string snippet;         // the directive's first line, trimmed
};

std::vector<CppInclude> parse_cpp_includes(const std::string& text);

// Where an include is looked up, in priority order: the repository root, every
// directory named `include`, and every directory named `src`. C++ has no packaging to
// read this out of -- the real answer lives in a build system's include path -- so these
// are the conventions a repository is laid out by, and an include that two of them could
// satisfy is reported as ambiguous rather than silently picked.
// `entries` is the provider's walk (repo-relative path -> is-directory).
std::vector<std::string> cpp_source_roots(const std::map<std::string, bool>& entries);

// The repo-relative file an include names, or "" when it is not in this repository.
// A quoted include is looked up beside the including file first, which is what the
// standard says and what makes `foo.cpp` -> `foo.h` unambiguous; an angled one goes
// straight to the roots. Ambiguity resolves to the first root and is reported.
std::string resolve_cpp_include(const std::string& from_rel, const CppInclude& inc,
                                const std::set<std::string>&    files,
                                const std::vector<std::string>& roots,
                                bool*                           ambiguous = nullptr);

// Resolves `includes` found in `from_rel`. One entry per distinct target, first
// occurrence wins, self-includes dropped.
std::vector<FileImport> resolve_cpp_includes(const std::string&              from_rel,
                                             const std::vector<CppInclude>&  includes,
                                             const std::set<std::string>&    files,
                                             const std::vector<std::string>& roots);

// -- build targets -------------------------------------------------------------
//
// What plays the part of a package. Python has `__init__.py` and npm has a manifest;
// C++ has neither, and the unit a C++ repository is actually built and reasoned in is
// the build target -- `add_library(rgv_core ...)`, `add_executable(rgv ...)`. The
// contract already has a `build_target` kind and the frontend already draws one.

struct CppTarget {
    std::string name;       // "rgv_core"
    std::string kind;       // executable | library
    std::string manifest;   // repo-relative CMakeLists.txt that declares it
    int         line = 0;   // 1-based line of the command
    std::string snippet;
    // Repo-relative, as written and normalized; entries the scanner could not resolve
    // (a `${VAR}` it does not know, a generator expression) are dropped rather than
    // guessed, and entries that name nothing in the tree fall out at ownership time.
    std::vector<std::string> sources;
    bool operator==(const CppTarget&) const = default;
};

// The targets one CMakeLists.txt declares. `dir_rel` is the directory it sits in, which
// is what a relative source path and `${CMAKE_CURRENT_SOURCE_DIR}` are relative to.
std::vector<CppTarget> parse_cmake_targets(const std::string& text, const std::string& dir_rel);

// Who owns what.
//
// A target owns the sources it lists and, transitively, the headers those sources
// include: a header is part of every translation unit that pulls it in, and in the
// usual C++ layout the .cpp files are listed in targets and the headers are in an
// `include/` tree that no build file mentions. Without the second half a repository's
// architecture is a set of boxes with no lines between them, because a .cpp never
// includes another .cpp.
//
// Ownership is a relation, not a tree: a header is reached by everything that uses it
// and a source file can be listed in two targets. The contract wants one containment
// parent and the rest as `owns` edges (3.1), so the claims are ordered and the first is
// the parent. A target that LISTS a file beats one that merely reaches it. Then a
// library beats an executable, because a header several targets compile belongs to the
// library among them -- a library exists to be included, an executable includes, and
// without this rule a one-file tool that happens to include the core's headers takes
// them off the core and every dependency in the view points the wrong way. Then the
// target whose sources are least scattered, which is what makes the provider's own
// headers the provider's rather than the test binary's that compiles two of them. Then
// the name, so a stream is deterministic.
struct CppOwnership {
    // file -> every target that claims it, the containment parent first.
    std::map<std::string, std::vector<std::string>> owners;
    // file -> the targets whose source list names it, same order. This is the
    // multi-owner case the contract means (3.1) and the only one worth an `owns` edge:
    // two build files both saying "this file is mine" is a fact about the build, where
    // a header being reached by everything that uses it is just what a header is.
    std::map<std::string, std::vector<std::string>> listed;
    // node (file OR directory) -> the target it parents onto. A directory whose owned
    // files all belong to one target moves as a whole, which is what keeps the
    // filesystem view's tree intact: the build target is hidden there, so a node under
    // it is drawn at the target's own directory, and moving the directory rather than
    // each file puts it back within one level of where it was.
    std::map<std::string, std::string> parent;
    // target -> the deepest directory that holds everything it owns. Where its node
    // sits, so a target whose code is one directory sits exactly on it.
    std::map<std::string, std::string> home;
    // Targets that own at least one file here, sorted. A target the build declares but
    // whose sources are not in this repository -- an interface library that is nothing
    // but compile flags -- is not one of them: the architecture view draws units of
    // code, and a box with nothing in it is not one.
    std::vector<std::string> names;

    const std::string* owner_of(const std::string& rel) const;
};

// `includes` is the resolved include graph: file -> the repo files it includes.
CppOwnership cpp_ownership(const std::vector<CppTarget>&                        targets,
                           const std::map<std::string, bool>&                   entries,
                           const std::map<std::string, std::vector<std::string>>& includes);

} // namespace rgv::watch
