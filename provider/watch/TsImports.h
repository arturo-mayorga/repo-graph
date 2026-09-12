// TypeScript and JavaScript import extraction, with Node's resolution rules.
//
// The same shape as the Python reader and for the same reasons: a line scanner rather
// than a parser, because an import is a statement at the top of a file and the cases a
// scanner cannot see -- a specifier built at run time, a path from a config -- are ones
// a dependency graph should not claim to know.
//
// What makes this different from Python is resolution. A specifier is extensionless and
// the file it names may be `.ts`, `.tsx`, `.js`, or a directory with an `index` in it;
// TypeScript compiled for ESM writes `./util.js` and means `./util.ts`; and a bare
// specifier is either a package in this repository or something in `node_modules`,
// which is not ours to report.
#pragma once

#include "FileImport.h"

#include <set>
#include <string>
#include <vector>

namespace rgv::watch {

// One module specifier, as written, with where it was written.
struct TsImport {
    std::string spec;              // "./util", "@acme/auth", "react"
    int         line = 0;
    std::string snippet;
    bool        type_only = false;  // `import type`: erased at run time, still a dependency
};

// Every specifier in `text`: static imports, re-exports (`export ... from`), `require`,
// and dynamic `import()`. Comments and strings are not specifiers, and a `//` inside a
// string is not a comment.
std::vector<TsImport> parse_ts_imports(const std::string& text);

// A package in this repository that a bare specifier can name.
struct TsPackage {
    std::string name;    // "@acme/auth"
    std::string rel;     // "packages/auth"
    std::string entry;   // package-relative entry from the manifest; may be empty
};

// The repo-relative file a specifier names, or empty when it is not in this repository.
// `node_modules`, Node's builtins and anything else unresolvable all come back empty --
// an edge to a node that does not exist is worse than no edge.
std::string resolve_ts_specifier(const std::string& from_rel, const std::string& spec,
                                 const std::set<std::string>&  files,
                                 const std::vector<TsPackage>& packages);

// Resolves `imports` found in `from_rel`. One entry per distinct target, first
// occurrence wins, self-imports dropped.
std::vector<FileImport> resolve_ts_imports(const std::string&            from_rel,
                                           const std::vector<TsImport>&  imports,
                                           const std::set<std::string>&  files,
                                           const std::vector<TsPackage>& packages);

} // namespace rgv::watch
