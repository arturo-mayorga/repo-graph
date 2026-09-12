// Python import extraction: the first language provider.
//
// Two separable steps, because they fail differently. Parsing turns source text into
// the import statements it contains and is a property of one file. Resolution turns a
// statement into a repo-relative file and depends on everything else in the tree --
// which files exist, where the source roots are -- so a file's edges can change without
// the file changing at all (a sibling appears; a package is added). Keeping the parsed
// statements and re-resolving them is what makes that cheap.
//
// A line-oriented scanner rather than a real parser, on purpose. Imports in Python are
// statements at the start of a logical line, and the cases a scanner gets wrong --
// imports built with `importlib`, `__import__`, or exec -- are exactly the ones a
// dependency graph should not claim to see. What it must not do is invent an edge from
// a comment or a string, so those are tracked.
//
// Unresolved imports produce nothing. `import os` and `import requests` are real
// dependencies, but not on anything in the repository, and an edge to a node that does
// not exist is worse than no edge (contract §6.4).
#pragma once

#include <functional>
#include "FileImport.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace rgv::watch {

struct ImportStmt {
    std::string              module;   // dotted, as written; "" for `from . import x`
    int                      level = 0;   // leading dots on a `from` import; 0 = absolute
    std::vector<std::string> names;    // `from m import a, b` -> {a, b}; `import m` -> {}
    // The local names the statement binds, one per item: `from m import a, b as c`
    // binds {a, c}; `import a.b as c` binds {c}; `import a.b` binds {a}. Resolution of
    // a use starts from a bound name, so aliases have to survive parsing.
    std::vector<std::string> bound;
    int                      line = 0;    // 1-based line the statement starts on
    std::string              snippet;  // the statement's first line, trimmed
};

std::vector<ImportStmt> parse_python_imports(const std::string& text);

// Where an absolute import is looked up, in priority order: the repository root, every
// package directory, and any `src` directory under either -- the src layout is the
// packaging guides' recommendation and the most common shape of a real Python package.
// `entries` is the provider's walk (repo-relative path -> is-directory).
std::vector<std::string> python_source_roots(const std::map<std::string, bool>& entries,
                                             const std::vector<std::string>&    package_dirs);

// Resolves `stmts` found in `from_rel` against the files that exist. One entry per
// distinct target, first occurrence wins; self-imports are dropped.
std::vector<FileImport> resolve_python_imports(const std::string&              from_rel,
                                               const std::vector<ImportStmt>&  stmts,
                                               const std::set<std::string>&    files,
                                               const std::vector<std::string>& roots);

// The file a module path names, from `from_rel`: `level` dots up, then `module`
// against every root. "" when it is not in the repository. Ambiguity is resolved to
// the first root and reported.
std::string resolve_python_module(const std::string& from_rel, const std::string& module,
                                  int level, const std::set<std::string>& files,
                                  const std::vector<std::string>& roots, bool* ambiguous = nullptr);

} // namespace rgv::watch

namespace rgv::watch {

// -- packages and the edges between them --------------------------------------
//
// A distribution (one pyproject.toml) is usually many Python packages, and the
// architecture of the code is how those packages import each other. So every
// directory with an `__init__.py` is a package node, nested under whatever contains
// it, and the file-level imports that cross a package boundary are aggregated into one
// `depends_on` edge per ordered pair, carrying the first crossing import as evidence.

struct PyPackage {
    std::string rel;      // repo-relative directory
    std::string module;   // dotted name relative to the source root that contains it
    bool operator==(const PyPackage&) const = default;
};

// Every regular package among `files`, sorted by path. Named relative to the longest
// source root that prefixes it, so `src/app/core` under root `src` is `app.core`.
std::vector<PyPackage> python_packages(const std::set<std::string>&    files,
                                       const std::vector<std::string>& roots);

struct PackageDep {
    std::string from, to;   // package ids
    std::string artifact;   // the importing file
    int         line = 0;
    std::string snippet;
    bool        heuristic = false;   // only when every contributing import is
};

// `file_imports` is (importing file, resolved import); `owner` maps a file to the id
// of the package that owns it, or "" for none. Imports inside one package produce
// nothing. Output is sorted by (from, to); evidence is the first contributing import in
// input order.
std::vector<PackageDep> aggregate_imports(
    const std::vector<std::pair<std::string, FileImport>>& file_imports,
    const std::function<std::string(const std::string&)>&  owner);

} // namespace rgv::watch

namespace rgv::watch {

// -- symbols and the files that use them ---------------------------------------
//
// The data a repository's parts exchange is named by its top-level classes and
// functions, and how a file uses one says what it does with it: constructing or
// invoking a name is a `calls` edge, every other mention -- a query by type, an
// annotation, an argument -- is a `references` edge. In an entity-component design
// that is exactly write versus read: the system that builds `CarPosition(...)` owns
// it, the systems that ask for `CarPosition` consume it, and the two are related by
// the component between them rather than by any import of each other.
//
// Only top-level definitions, and only uses reached through an import binding. A
// name defined and used inside one file is that file's business.

struct Definition {
    std::string name;
    std::string kind;   // class | function
    int         line = 0;
};

std::vector<Definition> parse_python_symbols(const std::string& text);

struct Use {
    std::string name;    // the bound local name
    std::string attr;    // `name.attr` -> attr; "" for a bare name
    bool        call = false;   // followed by `(`
    int         line = 0;
    std::string snippet;
};

// Every mention of a name bound by `stmts`, outside strings, comments, the import
// lines themselves, and definitions. Attribute positions (`x.name`) are not uses of
// `name`; `name.attr` is a use of `name` with `attr` set.
std::vector<Use> parse_python_uses(const std::string& text, const std::vector<ImportStmt>& stmts);

struct SymbolRef {
    std::string file;     // where the symbol is defined
    std::string symbol;   // its name
    std::string kind;     // calls | references
    int         line = 0;
    std::string snippet;
};

// Resolves uses in `from_rel` to definitions in other files, through the bindings and
// through re-exports (`from .car import CarState` in an `__init__.py`). One entry per
// (file, symbol, kind), first use as evidence; anything unresolved produces nothing.
std::vector<SymbolRef> resolve_python_uses(
    const std::string& from_rel, const std::vector<ImportStmt>& stmts,
    const std::vector<Use>& uses,
    const std::map<std::string, std::vector<ImportStmt>>& stmts_by_file,
    const std::map<std::string, std::vector<Definition>>& defs_by_file,
    const std::set<std::string>& files, const std::vector<std::string>& roots);

} // namespace rgv::watch
