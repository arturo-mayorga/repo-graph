// Package detection and declared dependencies.
//
// This is the part of the provider that knows about ecosystems, and it is deliberately
// the only part. Each reader is a small function over one manifest format; nothing else
// in `rgv-watch` knows that npm or Python exist, and adding an ecosystem is adding a
// reader rather than changing anything.
//
// Declared dependencies, not used ones. A manifest says what a package is allowed to
// depend on, which is exactly `confidence: "exact"` about the declaration and says
// nothing about whether a line of code imports it. Import-level truth is a different
// provider and a harder problem -- it needs a parser per language, where this needs a
// manifest reader per ecosystem.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace rgv::watch {

struct Dependency {
    std::string name;       // as written in the manifest
    std::string artifact;   // repo-relative manifest path, for evidence
    int         line = 0;   // 1-based; 0 when it could not be located
    std::string snippet;    // the manifest line, trimmed
    bool        dev = false;
};

struct Package {
    std::string id;         // "pkg:<name>"
    std::string name;       // what dependents refer to it by
    std::string rel;        // repo-relative directory; "" when the repo root is a package
    std::string manifest;   // repo-relative manifest path
    std::string provider;   // npm | python
    std::string version;

    std::vector<Dependency> deps;
};

// Finds every package under `root`. `entries` is the walk the provider already did --
// repo-relative path to is-directory -- so this costs a pass over the listing plus one
// read per manifest, not a second traversal.
std::vector<Package> scan_packages(const std::filesystem::path&        root,
                                   const std::map<std::string, bool>& entries);

// PEP 503 style: lowercase, and `-`, `_`, `.` all equivalent. Applied to both sides of a
// comparison so `my_pkg` and `my-pkg` resolve to the same package rather than silently
// becoming an external dependency that does not exist.
std::string normalize(std::string s);

} // namespace rgv::watch
