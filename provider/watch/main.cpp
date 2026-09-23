// rgv-watch -- the filesystem provider.
//
// Walks a checkout, emits it as a contract snapshot, then watches it and emits deltas.
// It speaks contract §6: newline-delimited JSON on stdout, one message per line, the
// first a `snapshot` and the rest events. Diagnostics go to stderr and are never
// protocol.
//
// Several adapters share the one stream (contract §6.2): the walk, which reports
// containment; the manifest readers, which report declared package dependencies; and
// one import extractor per language -- Python, TypeScript, C++ -- which report
// file-level `imports` edges and the blast radius they imply. The walk knows nothing
// about any language and is reused by every extractor; each extractor is its own module
// and the loop below only asks it two things -- parse this file, resolve everything --
// so the next language is another pair of functions, not a change to the loop.
//
// Linux-only (inotify). The transport and the walk are portable; only `Watcher` is not.

#include "CppImports.h"
#include "Ignore.h"
#include "Impact.h"
#include "Packages.h"
#include "PythonImports.h"
#include "TsImports.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void                       on_signal(int) { g_stop = 1; }

std::string language_of(const fs::path& p) {
    static const std::map<std::string, std::string> by_ext{
        {".py", "python"},   {".pyi", "python"},  {".ts", "typescript"},
        {".tsx", "typescript"}, {".js", "javascript"}, {".jsx", "javascript"},
        {".cpp", "cpp"},     {".cc", "cpp"},      {".cxx", "cpp"},
        {".h", "cpp"},       {".hpp", "cpp"},     {".c", "c"},
        {".rs", "rust"},     {".go", "go"},       {".java", "java"},
        {".rb", "ruby"},     {".cs", "csharp"},   {".json", "json"},
        {".md", "markdown"}, {".yml", "yaml"},    {".yaml", "yaml"},
        {".toml", "toml"},   {".sh", "shell"},    {".cmake", "cmake"}};
    auto it = by_ext.find(p.extension().string());
    return it == by_ext.end() ? std::string{} : it->second;
}

// Ids are opaque to the frontend (contract §2), but a readable convention costs nothing
// and makes a raw stream something a human can debug.
std::string file_id(const std::string& rel) { return "file:" + rel; }
std::string dir_id(const std::string& rel) { return rel.empty() ? "repo:root" : "dir:" + rel; }
std::string target_id(const std::string& name) { return "tgt:" + name; }

json node_json(const std::string& id, const std::string& kind, const std::string& name,
               const std::string& path, const std::string& parent,
               const std::string& language) {
    json n{{"id", id}, {"kind", kind}, {"name", name}, {"path", path},
           {"freshness", "current"}};
    if (parent.empty()) n["parent"] = nullptr;
    else                n["parent"] = parent;
    if (!language.empty()) n["language"] = language;
    return n;
}

// One line, flushed. A provider that buffers is a provider that looks hung.
void emit(const json& j) {
    std::cout << j.dump() << '\n' << std::flush;
}

struct Tree {
    // rel path -> is_directory. The whole state this provider keeps; a delta is the
    // difference between two of these.
    std::map<std::string, bool> entries;
};

std::string rel_of(const fs::path& root, const fs::path& p) {
    return fs::relative(p, root).generic_string();
}

// A derived directory is not noise in the graph, it is a second graph: everything
// inside it becomes nodes, packages, imports and an architecture of its own. What is
// skipped and why lives in `Ignore.h`; the walk just asks.
void walk(const fs::path& root, const fs::path& dir, Tree& tree,
          const rgv::watch::Ignores& ig) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string rel    = rel_of(root, e.path());
        const bool        is_dir = e.is_directory(ec);
        if (ig.skips(rel, is_dir)) continue;

        tree.entries[rel] = is_dir;
        if (is_dir) walk(root, e.path(), tree, ig);
    }
}

// rel directory -> the package that occupies it. A package node REPLACES the directory
// node at its path rather than sitting beside it, so `packages/auth` is one node, and
// everything inside it parents onto the package. That is what makes "which package owns
// this file" a walk up the containment tree rather than a path-prefix search (FR-11).
using PackageDirs = std::map<std::string, std::string>;   // rel dir -> package id

std::string parent_id_for(const std::string& rel, const PackageDirs& pkg_dirs) {
    const std::string parent = fs::path(rel).parent_path().generic_string();
    auto              it     = pkg_dirs.find(parent);
    return it != pkg_dirs.end() ? it->second : dir_id(parent);
}

json node_for(const fs::path& root, const std::string& rel, bool is_dir,
              const PackageDirs& pkg_dirs, const rgv::watch::CppOwnership& cpp) {
    const fs::path p = root / rel;
    // A build target claims what it builds, and that claim is the containment parent:
    // it is the only thing the architecture view can fold an import along.
    std::string parent_id = parent_id_for(rel, pkg_dirs);
    if (auto it = cpp.parent.find(rel); it != cpp.parent.end()) parent_id = target_id(it->second);

    if (is_dir) {
        // The package node for this directory is emitted separately, with its own kind
        // and attributes; there must not also be a directory node for the same path.
        if (pkg_dirs.count(rel)) return json();
        return node_json(dir_id(rel), "directory", p.filename().string(), rel, parent_id, {});
    }
    return node_json(file_id(rel), "file", p.filename().string(), rel, parent_id,
                     language_of(p));
}

// Packages, the external packages they name, and the depends_on edges between them.
//
// Direction is dependent -> dependency (contract §3.2); blast radius traverses it in
// reverse, which is what makes "I changed auth, what breaks" the natural query.
void emit_packages(const std::vector<rgv::watch::Package>& pkgs, json& nodes, json& edges,
                   std::vector<rgv::watch::GraphEdge>& pkg_edges,
                   const std::map<std::string, std::string>& module_nodes, long generation) {
    std::map<std::string, const rgv::watch::Package*> by_name;
    for (const auto& p : pkgs) by_name[rgv::watch::normalize(p.name)] = &p;

    for (const auto& p : pkgs) {
        json n = node_json(p.id, "package", p.name, p.home,
                           p.home.empty() ? "repo:root" : dir_id(fs::path(p.home).parent_path()
                                                                    .generic_string()),
                           p.provider == "python" ? "python" : "");
        json attrs{{"manifest", p.manifest}};
        if (!p.version.empty()) attrs["version"] = p.version;
        // The file that IS this package, when it has one: a distribution that absorbed
        // its same-named python package answers `from pkg import x` with that file.
        // A dependency on it is a dependency on the package as a unit, which is the one
        // kind of ancestor edge the frontend keeps (contract 6.4). The node's id, not
        // its path: the frontend is forbidden to parse an id (contract 2), and an
        // attribute carrying a path makes it build one instead, which holds only for as
        // long as every provider spells ids the way this one does.
        if (auto it = module_nodes.find(p.id); it != module_nodes.end()) {
            attrs["module_nodes"] = it->second;
        }
        n["attrs"] = attrs;
        nodes.push_back(std::move(n));
    }

    std::map<std::string, bool> externals;   // normalized name -> already emitted
    for (const auto& p : pkgs) {
        for (const auto& d : p.deps) {
            const std::string key = rgv::watch::normalize(d.name);
            auto              hit = by_name.find(key);

            std::string to_id;
            if (hit != by_name.end()) {
                if (hit->second == &p) continue;   // a package depending on itself is noise
                to_id = hit->second->id;
            } else {
                to_id = "ext:" + d.name;
                if (!externals[key]) {
                    externals[key] = true;
                    // Not in the repo, so it has no path and no containment parent: it is
                    // context for the graph, and the External filter exists to hide it.
                    nodes.push_back(
                        node_json(to_id, "external_package", d.name, "", "", ""));
                }
            }

            json e{{"id", "e:dep:" + p.name + "->" + d.name},
                   {"kind", "depends_on"},
                   {"from", p.id},
                   {"to", to_id},
                   {"provider", p.provider},
                   // The manifest says the dependency is declared, and that is all it
                   // says. Whether any code imports it is a different question, for a
                   // provider that reads code.
                   {"confidence", "exact"},
                   {"freshness", "current"},
                   {"valid_from", generation}};
            if (d.line > 0) {
                e["evidence"] = json{{"artifact", d.artifact}, {"line", d.line},
                                     {"snippet", d.snippet}};
            }
            pkg_edges.push_back(rgv::watch::GraphEdge{e["id"], p.id, to_id, generation, false});
            edges.push_back(std::move(e));
        }
    }
}

// The package that owns a file is the deepest package directory above it (FR-11).
std::string owning_package(const std::string& rel, const PackageDirs& pkg_dirs) {
    std::string dir = rel;
    for (;;) {
        const auto slash = dir.rfind('/');
        dir              = slash == std::string::npos ? std::string{} : dir.substr(0, slash);
        if (auto it = pkg_dirs.find(dir); it != pkg_dirs.end()) return it->second;
        if (dir.empty()) return {};
    }
}

// -- python imports -----------------------------------------------------------
//
// Parsed statements are kept per file and re-resolved as a whole whenever the tree or
// any file changes. Resolution is a handful of set lookups per statement, so doing all
// of it again is cheaper than working out which files a change could affect -- and a
// file's edges genuinely can change without the file being touched, when a sibling
// module appears or a target is deleted.

bool is_python(const std::string& rel) {
    return rel.size() > 3 && rel.compare(rel.size() - 3, 3, ".py") == 0;
}

bool is_ts(const std::string& rel) {
    for (const char* ext : {".ts", ".tsx", ".mts", ".cts", ".js", ".jsx", ".mjs", ".cjs"}) {
        const std::size_t n = std::strlen(ext);
        if (rel.size() > n && rel.compare(rel.size() - n, n, ext) == 0) return true;
    }
    return false;
}

bool is_cpp(const std::string& rel) {
    for (const char* ext : {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx",
                            ".inl", ".ipp"}) {
        const std::size_t n = std::strlen(ext);
        if (rel.size() > n && rel.compare(rel.size() - n, n, ext) == 0) return true;
    }
    return false;
}

// The build file, which is where a C++ repository says what its units are.
bool is_cmake(const std::string& rel) {
    const std::string name = fs::path(rel).filename().string();
    return name == "CMakeLists.txt";
}

bool is_source(const std::string& rel) { return is_python(rel) || is_ts(rel) || is_cpp(rel); }

// Which reader produced an edge, so the inspector can say and the contract's rule that
// a `provider` names an announced adapter holds (section 6.2).
const char* imports_provider(const std::string& rel) {
    if (is_ts(rel)) return "ts-imports";
    if (is_cpp(rel)) return "cpp-imports";
    return "python-imports";
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream os;
    os << in.rdbuf();
    return os.str();
}

// The repository's own statement about which of its directories are derived. Re-read
// on every re-walk rather than cached: editing `.gitignore` is exactly the moment the
// answer should change, and the file is one read next to a full tree walk.
rgv::watch::Ignores ignores_under(const fs::path& root) {
    rgv::watch::Ignores ig;
    ig.add_gitignore(read_text(root / ".gitignore"));
    return ig;
}

struct ImportEdge {
    std::string from, to;   // repo-relative files
    int         line = 0;
    std::string snippet;
    bool        heuristic  = false;
    long        valid_from = 0;
};

std::string import_edge_id(const std::string& from, const std::string& to) {
    return "e:imp:" + from + "->" + to;
}

json import_edge_json(const std::string& id, const ImportEdge& e) {
    return json{{"id", id},
                {"kind", "imports"},
                {"from", file_id(e.from)},
                {"to", file_id(e.to)},
                {"provider", imports_provider(e.from)},
                {"confidence", e.heuristic ? "heuristic" : "exact"},
                {"freshness", "current"},
                {"valid_from", e.valid_from},
                {"evidence", json{{"artifact", e.from}, {"line", e.line}, {"snippet", e.snippet}}}};
}

struct ImportIndex {
    std::map<std::string, std::vector<rgv::watch::ImportStmt>>  parsed;      // python
    std::map<std::string, std::vector<rgv::watch::TsImport>>    ts_parsed;   // typescript
    std::map<std::string, std::vector<rgv::watch::CppInclude>>  cpp_parsed;  // c++
    std::map<std::string, ImportEdge>                           edges;       // id -> file edge
    std::vector<std::string>                                    roots;       // python source roots
    std::vector<std::string>                                    cpp_roots;
    std::vector<rgv::watch::TsPackage>                          ts_packages;

    struct DepEdge {
        rgv::watch::PackageDep dep;
        long                   valid_from = 0;
    };
    std::map<std::string, DepEdge> deps;   // id -> aggregated package edge

    void parse_text(const std::string& rel, const std::string& text) {
        parsed[rel] = rgv::watch::parse_python_imports(text);
    }
    void parse_ts(const std::string& rel, const std::string& text) {
        ts_parsed[rel] = rgv::watch::parse_ts_imports(text);
    }
    void parse_cpp(const std::string& rel, const std::string& text) {
        cpp_parsed[rel] = rgv::watch::parse_cpp_includes(text);
    }
    void forget(const std::string& rel) {
        parsed.erase(rel);
        ts_parsed.erase(rel);
        cpp_parsed.erase(rel);
    }

    static std::set<std::string> files_in(const Tree& tree) {
        std::set<std::string> files;
        for (const auto& [rel, is_dir] : tree.entries) {
            if (!is_dir) files.insert(rel);
        }
        return files;
    }

    // Who includes what, which is what decides which build target owns a header. It
    // costs one resolution pass more than `resolve` does its own -- the alternative is
    // to resolve the includes before anything can be said about ownership and then to
    // remember them, and a repository's includes are a few thousand set lookups.
    std::map<std::string, std::vector<std::string>> cpp_graph(const Tree& tree) const {
        const auto                                      files = files_in(tree);
        std::map<std::string, std::vector<std::string>> out;
        for (const auto& [rel, incs] : cpp_parsed) {
            for (const auto& fi : rgv::watch::resolve_cpp_includes(rel, incs, files, cpp_roots)) {
                out[rel].push_back(fi.to);
            }
        }
        return out;
    }

    // Re-resolves everything and reports what moved: new edges carry `generation`,
    // surviving ones keep the generation they first appeared in, and a surviving edge
    // whose evidence or confidence changed is a replacement, not a new edge.
    void resolve(const Tree& tree, long generation,
                 const std::function<std::string(const std::string&)>& owner, json& added,
                 json& removed, json& updated) {
        const std::set<std::string> files = files_in(tree);
        std::map<std::string, ImportEdge>                        next;
        std::vector<std::pair<std::string, rgv::watch::FileImport>> flat;
        auto take = [&](const std::string& rel, const std::vector<rgv::watch::FileImport>& found) {
            for (const auto& fi : found) {
                const std::string id = import_edge_id(rel, fi.to);
                ImportEdge        e{rel, fi.to, fi.line, fi.snippet, fi.ambiguous, generation};
                if (auto old = edges.find(id); old != edges.end()) e.valid_from = old->second.valid_from;
                next[id] = std::move(e);
                flat.emplace_back(rel, fi);
            }
        };
        for (const auto& [rel, stmts] : parsed) {
            take(rel, rgv::watch::resolve_python_imports(rel, stmts, files, roots));
        }
        for (const auto& [rel, imports] : ts_parsed) {
            take(rel, rgv::watch::resolve_ts_imports(rel, imports, files, ts_packages));
        }
        for (const auto& [rel, includes] : cpp_parsed) {
            take(rel, rgv::watch::resolve_cpp_includes(rel, includes, files, cpp_roots));
        }
        for (const auto& [id, e] : edges) {
            if (!next.count(id)) removed.push_back(id);
        }
        for (const auto& [id, e] : next) {
            auto old = edges.find(id);
            if (old == edges.end()) added.push_back(import_edge_json(id, e));
            else if (old->second.heuristic != e.heuristic || old->second.line != e.line ||
                     old->second.snippet != e.snippet) {
                updated.push_back(import_edge_json(id, e));
            }
        }
        edges = std::move(next);

        // The level above the files: one edge per ordered pair of owners that an import
        // crosses between. An owner is the innermost package above the file (FR-11), or
        // the build target that builds it where a language has no packages of its own.
        std::map<std::string, DepEdge> next_deps;
        for (auto& d : rgv::watch::aggregate_imports(flat, owner)) {
            const std::string id = "e:impdep:" + d.from + "->" + d.to;
            DepEdge           de{std::move(d), generation};
            if (auto old = deps.find(id); old != deps.end()) de.valid_from = old->second.valid_from;
            next_deps[id] = std::move(de);
        }
        for (const auto& [id, d] : deps) {
            if (!next_deps.count(id)) removed.push_back(id);
        }
        for (const auto& [id, d] : next_deps) {
            auto old = deps.find(id);
            if (old == deps.end()) added.push_back(dep_edge_json(id, d));
            else if (old->second.dep.heuristic != d.dep.heuristic ||
                     old->second.dep.artifact != d.dep.artifact || old->second.dep.line != d.dep.line) {
                updated.push_back(dep_edge_json(id, d));
            }
        }
        deps = std::move(next_deps);
    }

    static json dep_edge_json(const std::string& id, const DepEdge& d) {
        return json{{"id", id},
                    {"kind", "depends_on"},
                    {"from", d.dep.from},
                    {"to", d.dep.to},
                    {"provider", imports_provider(d.dep.artifact)},
                    {"confidence", d.dep.heuristic ? "heuristic" : "exact"},
                    {"freshness", "current"},
                    {"valid_from", d.valid_from},
                    {"evidence", json{{"artifact", d.dep.artifact}, {"line", d.dep.line},
                                      {"snippet", d.dep.snippet}}}};
    }

    std::vector<rgv::watch::GraphEdge> graph_edges() const {
        std::vector<rgv::watch::GraphEdge> out;
        for (const auto& [id, e] : edges) {
            out.push_back({id, file_id(e.from), file_id(e.to), e.valid_from, e.heuristic});
        }
        return out;
    }
    std::vector<rgv::watch::GraphEdge> dep_graph_edges() const {
        std::vector<rgv::watch::GraphEdge> out;
        for (const auto& [id, d] : deps) {
            out.push_back({id, d.dep.from, d.dep.to, d.valid_from, d.dep.heuristic});
        }
        return out;
    }
};

// -- build targets ------------------------------------------------------------
//
// What plays the part of a package.
//
// Python has `__init__.py` and npm has a manifest; C++ has nothing of the kind, and a
// repository of it is still made of units -- the things the build produces. So the unit
// is the build target: `add_library(rgv_core ...)`, `add_executable(rgv ...)`, read out
// of CMakeLists.txt, which is the only place a C++ repository says what it is made of.
//
// The decision that costs something is containment. The architecture view draws
// `imports` edges folded onto whatever CONTAINS their endpoints, so a file with no
// package and no target above it contributes nothing to that view but a line from the
// repository node, which the view drops on purpose. Boxes with no lines between them
// are not an architecture, so the target has to be the containment parent of what it
// builds -- and, because a .cpp never includes another .cpp, of the headers its sources
// reach as well, or every line would end in an `include/` tree that no build file
// mentions and no target owns.
//
// A target is not a directory, and that is the price. Its sources come from several
// directories and one file can be listed in two targets, so this is ownership rather
// than a path, and the contract's answer is one parent and an `owns` edge for every
// further claim (§3.1) -- never two parents. Where a whole directory belongs to one
// target the directory moves rather than each file in it, which keeps the filesystem
// view -- which does not draw build targets -- showing that directory within a level of
// where it sits instead of spilling every file in it onto the target's own directory.

struct TargetIndex {
    std::map<std::string, std::vector<rgv::watch::CppTarget>> parsed;   // CMakeLists -> targets
    rgv::watch::CppOwnership                                  own;

    struct OwnsEdge {
        std::string target, file;
        long        valid_from = 0;
    };
    std::map<std::string, OwnsEdge> owns;   // id -> edge

    void parse(const std::string& rel, const std::string& text) {
        auto ts = rgv::watch::parse_cmake_targets(text, fs::path(rel).parent_path().generic_string());
        for (auto& t : ts) t.manifest = rel;
        parsed[rel] = std::move(ts);
    }
    void forget(const std::string& rel) { parsed.erase(rel); }

    std::vector<rgv::watch::CppTarget> declared() const {
        std::vector<rgv::watch::CppTarget> out;
        for (const auto& [rel, ts] : parsed) out.insert(out.end(), ts.begin(), ts.end());
        return out;
    }
    const rgv::watch::CppTarget* find(const std::string& name) const {
        for (const auto& [rel, ts] : parsed) {
            for (const auto& t : ts) {
                if (t.name == name) return &t;
            }
        }
        return nullptr;
    }
    rgv::watch::CppOwnership ownership(
        const Tree& tree, const std::map<std::string, std::vector<std::string>>& graph) const {
        return rgv::watch::cpp_ownership(declared(), tree.entries, graph);
    }
    static bool has(const rgv::watch::CppOwnership& o, const std::string& name) {
        return std::find(o.names.begin(), o.names.end(), name) != o.names.end();
    }
    std::string owner_id(const std::string& rel) const {
        const std::string* t = own.owner_of(rel);
        return t == nullptr ? std::string{} : target_id(*t);
    }

    // The files that ARE the target (contract 6.4): what another translation unit
    // includes when it depends on this target as a unit, rather than reaching into
    // something that merely lives inside it.
    //
    // C++ declares no such thing, so the only signal that is not a guess is the one
    // convention where the build itself says the name: a file whose stem is the
    // target's own -- `core.h` and `core.cpp` for `core`. Both halves are emitted,
    // because in C++ both halves are the module; that is the case the attribute is
    // plural for, and the one a single path could not have expressed.
    //
    // Nothing weaker counts. No case folding, no `lib`/`_lib` stripping, and in
    // particular not "the target's only source": `add_executable(app app/main.cpp)`
    // builds `app` out of a file that is not `app`, and claiming otherwise would fold
    // away a real edge, since the frontend drops an edge into a unit's own module as
    // containment restated. A target with no such file emits nothing, which the
    // contract asks for and which is the failure this attribute is allowed to have.
    static std::string module_nodes_of(const rgv::watch::CppOwnership& o,
                                       const std::string&             name) {
        std::string out;
        // `owners` is a std::map, so this is path order: the stream stays byte-stable
        // across runs, and the header sorts ahead of the source in the usual layout.
        for (const auto& [file, claimants] : o.owners) {
            if (fs::path(file).stem().string() != name) continue;
            if (std::find(claimants.begin(), claimants.end(), name) == claimants.end()) continue;
            if (!out.empty()) out += ',';
            out += file_id(file);
        }
        return out;
    }

    json node_json_for(const rgv::watch::CppOwnership& o, const std::string& name,
                       const PackageDirs& pkg_dirs) const {
        // The node stands where its code does -- the deepest directory holding
        // everything it owns -- so a target whose sources are one directory sits on it.
        const std::string home = o.home.count(name) ? o.home.at(name) : std::string{};
        auto              it   = pkg_dirs.find(home);
        json              n    = node_json(target_id(name), "build_target", name, home,
                                           it != pkg_dirs.end() ? it->second : dir_id(home), "cpp");
        json attrs;
        if (const auto* t = find(name)) {
            attrs["type"]     = t->kind;
            attrs["manifest"] = t->manifest;
        }
        if (const std::string mods = module_nodes_of(o, name); !mods.empty()) {
            attrs["module_nodes"] = mods;
        }
        n["attrs"] = attrs;
        return n;
    }

    json owns_edge_json(const std::string& id, const OwnsEdge& e) const {
        json j{{"id", id},
               {"kind", "owns"},
               {"from", target_id(e.target)},
               {"to", file_id(e.file)},
               {"provider", "cpp-imports"},
               {"confidence", "exact"},
               {"freshness", "current"},
               {"valid_from", e.valid_from}};
        if (const auto* t = find(e.target)) {
            j["evidence"] = json{{"artifact", t->manifest}, {"line", t->line}, {"snippet", t->snippet}};
        }
        return j;
    }

    // The claims past the first. A file two build files both name is genuinely owned
    // twice, and that is what `owns` is for (contract §3.1); a header being reached by
    // everything that uses it is just what a header is, and is not reported.
    void resolve_owns(const rgv::watch::CppOwnership& o, long generation, json& added,
                      json& removed) {
        std::map<std::string, OwnsEdge> next;
        for (const auto& [file, listers] : o.listed) {
            for (std::size_t i = 1; i < listers.size(); ++i) {
                if (!has(o, listers[i])) continue;
                const std::string id = "e:owns:" + target_id(listers[i]) + "->" + file_id(file);
                OwnsEdge          e{listers[i], file, generation};
                if (auto old = owns.find(id); old != owns.end()) e.valid_from = old->second.valid_from;
                next[id] = std::move(e);
            }
        }
        for (const auto& [id, e] : owns) {
            if (!next.count(id)) removed.push_back(id);
        }
        for (const auto& [id, e] : next) {
            if (!owns.count(id)) added.push_back(owns_edge_json(id, e));
        }
        owns = std::move(next);
    }
};

// -- symbols ------------------------------------------------------------------
//
// Top-level definitions and the files that use them across a file boundary. Only a
// symbol something else uses becomes a node: one nobody imports is a detail of its
// file, and the file node already stands for it. Like the import edges, everything is
// re-resolved from parsed state after any change and reported as a difference.

std::string symbol_id(const std::string& file, const std::string& name) {
    return "sym:" + file + "#" + name;
}

struct SymbolIndex {
    std::map<std::string, std::vector<rgv::watch::Definition>> defs;   // rel -> definitions
    std::map<std::string, std::vector<rgv::watch::Use>>        uses;   // rel -> uses

    struct SymNode {
        std::string file, name, kind;
        int         line = 0;
        bool operator==(const SymNode&) const = default;
    };
    struct SymEdge {
        std::string            from;   // the using file
        rgv::watch::SymbolRef  ref;
        long                   valid_from = 0;
    };
    std::map<std::string, SymNode> nodes;   // id -> symbol
    std::map<std::string, SymEdge> edges;   // id -> edge

    void parse(const std::string& rel, const std::string& text,
               const std::vector<rgv::watch::ImportStmt>& stmts) {
        defs[rel] = rgv::watch::parse_python_symbols(text);
        uses[rel] = rgv::watch::parse_python_uses(text, stmts);
    }
    void forget(const std::string& rel) { defs.erase(rel); uses.erase(rel); }

    static json node_json_for(const std::string& id, const SymNode& n) {
        json j = node_json(id, "symbol", n.name, n.file, file_id(n.file), "python");
        j["attrs"] = json{{"kind", n.kind}, {"line", std::to_string(n.line)}};
        return j;
    }
    static json edge_json_for(const std::string& id, const SymEdge& e) {
        return json{{"id", id},
                    {"kind", e.ref.kind},
                    {"from", file_id(e.from)},
                    {"to", symbol_id(e.ref.file, e.ref.symbol)},
                    {"provider", "python-imports"},
                    {"confidence", "exact"},
                    {"freshness", "current"},
                    {"valid_from", e.valid_from},
                    {"evidence", json{{"artifact", e.from}, {"line", e.ref.line},
                                      {"snippet", e.ref.snippet}}}};
    }

    void resolve(const ImportIndex& imports, const Tree& tree, long generation,
                 json& added_nodes, json& removed_nodes, json& updated_nodes,
                 json& added_edges, json& removed_edges, json& updated_edges) {
        std::set<std::string> files;
        for (const auto& [rel, is_dir] : tree.entries) {
            if (!is_dir) files.insert(rel);
        }
        std::map<std::string, SymNode> next_nodes;
        std::map<std::string, SymEdge> next_edges;
        for (const auto& [rel, us] : uses) {
            auto st = imports.parsed.find(rel);
            if (st == imports.parsed.end()) continue;
            for (auto& ref : rgv::watch::resolve_python_uses(rel, st->second, us, imports.parsed, defs,
                                                             files, imports.roots)) {
                const std::string sid = symbol_id(ref.file, ref.symbol);
                if (!next_nodes.count(sid)) {
                    SymNode n{ref.file, ref.symbol, "", 0};
                    for (const auto& d : defs.at(ref.file)) {
                        if (d.name == ref.symbol) { n.kind = d.kind; n.line = d.line; break; }
                    }
                    next_nodes[sid] = std::move(n);
                }
                const std::string eid = "e:" + ref.kind + ":" + rel + "->" + sid;
                SymEdge           e{rel, std::move(ref), generation};
                if (auto old = edges.find(eid); old != edges.end()) e.valid_from = old->second.valid_from;
                next_edges[eid] = std::move(e);
            }
        }
        for (const auto& [id, n] : nodes) {
            if (!next_nodes.count(id)) removed_nodes.push_back(id);
        }
        for (const auto& [id, n] : next_nodes) {
            auto old = nodes.find(id);
            if (old == nodes.end()) added_nodes.push_back(node_json_for(id, n));
            else if (!(old->second == n)) updated_nodes.push_back(node_json_for(id, n));
        }
        for (const auto& [id, e] : edges) {
            if (!next_edges.count(id)) removed_edges.push_back(id);
        }
        for (const auto& [id, e] : next_edges) {
            auto old = edges.find(id);
            if (old == edges.end()) added_edges.push_back(edge_json_for(id, e));
            else if (old->second.ref.line != e.ref.line || old->second.ref.snippet != e.ref.snippet) {
                updated_edges.push_back(edge_json_for(id, e));
            }
        }
        nodes = std::move(next_nodes);
        edges = std::move(next_edges);
    }

    std::vector<rgv::watch::GraphEdge> graph_edges() const {
        std::vector<rgv::watch::GraphEdge> out;
        for (const auto& [id, e] : edges) {
            out.push_back({id, file_id(e.from), symbol_id(e.ref.file, e.ref.symbol), e.valid_from, false});
        }
        return out;
    }
    // The symbols a set of changed files defines, which is what a symbol-level blast
    // radius grows from.
    std::vector<std::string> seeds_in(const std::set<std::string>& changed) const {
        std::vector<std::string> out;
        for (const auto& [id, n] : nodes) {
            if (changed.count(n.file)) out.push_back(id);
        }
        return out;
    }
};

// -- python packages ----------------------------------------------------------
//
// Every directory with an `__init__.py` is a package node, nested under whatever
// contains it, and it replaces the directory node at its path exactly as a manifest
// package does. That is what makes a single-distribution repository -- one
// pyproject.toml, a dozen packages -- an architecture rather than one box.

std::string pypkg_id(const std::string& rel) { return "pypkg:" + rel; }

std::vector<rgv::watch::PyPackage> detect_python_packages(const Tree&                     tree,
                                                          const std::vector<std::string>& roots) {
    std::set<std::string> files;
    for (const auto& [rel, is_dir] : tree.entries) {
        if (!is_dir) files.insert(rel);
    }
    return rgv::watch::python_packages(files, roots);
}

// A distribution and its top-level package almost always share a name: `elevators` the
// pyproject.toml and `elevators` the directory under `src/`. They are one unit, and two
// boxes carrying the same label is not an architecture -- on this repo the duplicate
// accounted for 8 of 27 package-level edges and more than doubled the crossings. So the
// python package is dropped and its contents reparent onto the distribution, which is
// what actually owns them.
//
// And the surviving node moves to where the twin stood. A manifest says where a build
// starts, not where the code is: `pyproject.toml` sits at the repository root and
// declares `src/elevators`. Leaving the node on the manifest's directory makes it the
// repository wearing a package's name -- `tests/`, `docs/` and `tools/` all fall inside
// it, and on this repo 132 of the 158 imports credited to the architecture came from
// the test suite, three of the cycles with them. Seating it over its code is what makes
// the containment tree's answer to "which package owns this file" (FR-11) true.
std::vector<rgv::watch::PyPackage> drop_distribution_twins(
    std::vector<rgv::watch::PyPackage> py, std::vector<rgv::watch::Package>& packages,
    const PackageDirs& manifest_dirs, std::map<std::string, std::string>& module_nodes) {
    std::map<std::string, std::string> name_of;
    for (const auto& p : packages) name_of[p.id] = p.name;

    // Collected rather than applied in the loop: the twin is found through the manifest
    // directories, so moving a package while still reading them would move the ground.
    std::map<std::string, std::string> home_of;
    py.erase(std::remove_if(py.begin(), py.end(),
                            [&](const rgv::watch::PyPackage& p) {
                                const std::string owner = owning_package(p.rel, manifest_dirs);
                                auto              it    = name_of.find(owner);
                                const bool        twin =
                                    it != name_of.end() && rgv::watch::normalize(it->second) ==
                                                               rgv::watch::normalize(p.module);
                                // The distribution inherits the twin's own module, so
                                // `from elevators import x` still resolves to a node.
                                if (twin) {
                                    module_nodes[owner] = file_id(p.rel + "/__init__.py");
                                    home_of[owner]     = p.rel;
                                }
                                return twin;
                            }),
             py.end());
    for (auto& p : packages) {
        if (auto it = home_of.find(p.id); it != home_of.end()) p.home = it->second;
    }
    return py;
}

json pypkg_node(const rgv::watch::PyPackage& p, const PackageDirs& pkg_dirs) {
    json n = node_json(pypkg_id(p.rel), "package", p.module, p.rel, parent_id_for(p.rel, pkg_dirs),
                       "python");
    // Plural in the contract, singular here: Python's unit is one `__init__.py`, which
    // is the special case the attribute is named for rather than the general one.
    n["attrs"] = json{{"module", p.module},
                      {"package", "python"},
                      {"module_nodes", file_id(p.rel + "/__init__.py")}};
    return n;
}

// -- impact -------------------------------------------------------------------

json impact_json(const char* level, std::vector<std::string> edge_kinds, long generation, long baseline,
                 const std::vector<std::string>& seeds, const std::vector<rgv::watch::ImpactHit>& hits) {
    json nodes = json::array();
    for (const auto& h : hits) {
        json paths = json::array();
        if (!h.path.empty()) paths.push_back(json{{"edges", h.path}});
        nodes.push_back(json{{"node_id", h.node},
                             {"min_distance", h.distance},
                             {"direct", h.distance == 1},
                             {"changed", h.distance == 0},
                             {"freshness", "current"},
                             {"cause", h.dependency_added ? "dependency_added" : "implementation"},
                             {"paths", paths}});
    }
    return json{{"t_ms", 0},
                {"type", "impact.updated"},
                {"generation", generation},
                {"level", level},
                {"baseline_generation", baseline},
                {"seed_nodes", seeds},
                {"filters", json{{"max_depth", 8}, {"edge_kinds", edge_kinds},
                                 {"include_heuristic", false}}},
                {"impacted_nodes", nodes}};
}

// -- inotify ------------------------------------------------------------------
//
// Recursive watching is not a thing inotify offers, so every directory gets its own
// watch and new directories are added as they appear.
class Watcher {
public:
    // The same rules as the walk, and the same object: a watch on a tree the walk does
    // not report is a wakeup per build artifact and nothing on screen to show for it.
    Watcher(const fs::path& root, const rgv::watch::Ignores& ig) : root_(root), ig_(ig) {
        fd_ = ::inotify_init1(IN_NONBLOCK);
        if (fd_ < 0) throw std::runtime_error(std::string("inotify_init1: ") + std::strerror(errno));
    }
    ~Watcher() { if (fd_ >= 0) ::close(fd_); }

    int fd() const { return fd_; }

    void add_recursive(const fs::path& dir) {
        add(dir);
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
            if (!e.is_directory(ec)) continue;
            if (ig_.skips(rel_of(root_, e.path()), true)) continue;
            add(e.path());
        }
    }

    // Returns true if anything at all happened, so the caller knows to re-scan.
    bool drain() {
        char    buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
        bool    any = false;
        for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n <= 0) break;
            any = true;
            for (char* p = buf; p < buf + n;) {
                auto* ev = reinterpret_cast<struct inotify_event*>(p);
                // A new directory needs its own watch, or everything inside it is
                // invisible until the next full restart.
                if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR) && ev->len > 0) {
                    auto it = paths_.find(ev->wd);
                    if (it != paths_.end()) add_recursive(it->second / ev->name);
                }
                p += sizeof(struct inotify_event) + ev->len;
            }
        }
        return any;
    }

private:
    void add(const fs::path& dir) {
        const int wd = ::inotify_add_watch(
            fd_, dir.c_str(),
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);
        if (wd >= 0) paths_[wd] = dir;
    }

    fs::path                          root_;
    const rgv::watch::Ignores&        ig_;
    int                               fd_ = -1;
    std::unordered_map<int, fs::path> paths_;
};

} // namespace

int main(int argc, char** argv) {
    fs::path root = fs::current_path();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--root" || a == "-C") && i + 1 < argc) root = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                         "rgv-watch -- filesystem provider for the Live Repository Impact Graph\n"
                         "usage: rgv-watch [--root <dir>]\n"
                         "writes contract NDJSON on stdout; diagnostics on stderr\n");
            return 0;
        }
    }

    std::error_code ec;
    root = fs::canonical(root, ec);
    if (ec) {
        std::fprintf(stderr, "rgv-watch: cannot resolve root: %s\n", ec.message().c_str());
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    // A dead frontend closes our stdout; without this the first write kills us with a
    // signal instead of a clean exit.
    std::signal(SIGPIPE, SIG_IGN);

    rgv::watch::Ignores ignores = ignores_under(root);
    Tree                tree;
    walk(root, root, tree, ignores);

    // Content changes do not show up in the tree listing, so mtimes are tracked
    // alongside it. Seeded from the baseline, or every file would report as modified
    // the first time anything at all happened.
    std::map<std::string, long long> mtimes;
    auto stamp_of = [&](const std::string& rel) -> long long {
        std::error_code wec;
        const auto      t = fs::last_write_time(root / rel, wec);
        return wec ? 0 : static_cast<long long>(t.time_since_epoch().count());
    };
    for (const auto& [rel, is_dir] : tree.entries) {
        if (!is_dir) mtimes[rel] = stamp_of(rel);
    }

    long generation = 100;

    std::vector<rgv::watch::Package>   packages;
    PackageDirs                        manifest_dirs;   // manifest packages only
    PackageDirs                        pkg_dirs;        // ... plus python packages
    std::vector<rgv::watch::PyPackage> pypkgs;
    std::map<std::string, std::string> dist_module_nodes;   // manifest package id -> its module's node id
    std::vector<rgv::watch::GraphEdge> pkg_edges;
    ImportIndex                        imports;
    TargetIndex                        targets;
    SymbolIndex                        symbols;
    std::set<std::string>              changed;   // files touched since the baseline
    const long                         baseline = generation;

    auto parse_source = [&](const std::string& rel) {
        const std::string text = read_text(root / rel);
        if (is_python(rel)) {
            imports.parse_text(rel, text);
            symbols.parse(rel, text, imports.parsed[rel]);
        } else if (is_ts(rel)) {
            // No symbol extraction for TypeScript yet: its export forms are varied
            // enough that a line scanner would start guessing, and a wrong symbol edge
            // is worse than a missing one.
            imports.parse_ts(rel, text);
        } else if (is_cpp(rel)) {
            // Nor for C++: a definition is spread over a header and a translation unit,
            // and which one a use names is a question for a parser.
            imports.parse_cpp(rel, text);
        }
    };
    auto reroot = [&](const Tree& t) {
        std::vector<std::string> dirs;
        for (const auto& p : packages) dirs.push_back(p.rel);
        imports.roots = rgv::watch::python_source_roots(t.entries, dirs);

        // What a bare specifier is allowed to name: the packages this repository
        // declares. Everything else is node_modules, and not ours to report.
        imports.ts_packages.clear();
        for (const auto& p : packages) {
            if (p.provider == "npm") imports.ts_packages.push_back({p.name, p.rel, p.entry});
        }

        // C++ has no packaging to read an include path out of, so the conventions a
        // repository is laid out by stand in for it.
        imports.cpp_roots = rgv::watch::cpp_source_roots(t.entries);
    };
    // What an import is attributed to one level up: the package that owns the file
    // (FR-11), or the build target that builds it where the language has no packages.
    auto owner_of = [&](const std::string& f) {
        const std::string t = targets.owner_id(f);
        return t.empty() ? owning_package(f, pkg_dirs) : t;
    };
    // Containment, so a manifest package is keyed by where its code is. `manifest_dirs`
    // stays keyed by the manifest's own directory: that is what a module path resolves
    // against, and what the twin above is recognised through.
    auto merge_dirs = [&](const std::vector<rgv::watch::PyPackage>& py) {
        PackageDirs all;
        for (const auto& p : packages) all[p.home] = p.id;
        for (const auto& p : py) all[p.rel] = pypkg_id(p.rel);
        return all;
    };

    // -- baseline
    {
        json nodes = json::array();
        json edges = json::array();
        nodes.push_back(node_json("repo:root", "repository", root.filename().string(), "", {}, {}));

        packages = rgv::watch::scan_packages(root, tree.entries);
        for (const auto& p : packages) manifest_dirs[p.rel] = p.id;
        reroot(tree);
        pypkgs   = drop_distribution_twins(detect_python_packages(tree, imports.roots), packages,
                                           manifest_dirs, dist_module_nodes);
        pkg_dirs = merge_dirs(pypkgs);

        // Before the nodes, because a C++ file's containment parent is the target that
        // builds it and that is only known once the includes are resolved.
        for (const auto& [rel, is_dir] : tree.entries) {
            if (is_dir) continue;
            if (is_source(rel)) parse_source(rel);
            if (is_cmake(rel)) targets.parse(rel, read_text(root / rel));
        }
        targets.own = targets.ownership(tree, imports.cpp_graph(tree));

        for (const auto& [rel, is_dir] : tree.entries) {
            json n = node_for(root, rel, is_dir, pkg_dirs, targets.own);
            if (!n.is_null()) nodes.push_back(std::move(n));
        }
        emit_packages(packages, nodes, edges, pkg_edges, dist_module_nodes, generation);
        for (const auto& p : pypkgs) nodes.push_back(pypkg_node(p, pkg_dirs));
        for (const auto& name : targets.own.names) {
            nodes.push_back(targets.node_json_for(targets.own, name, pkg_dirs));
        }

        json removed = json::array();
        json updated = json::array();
        imports.resolve(tree, generation, owner_of, edges, removed, updated);
        targets.resolve_owns(targets.own, generation, edges, removed);
        symbols.resolve(imports, tree, generation, nodes, removed, updated, edges, removed, updated);

        json snap{{"schema", "rgv.snapshot/1"},
                  {"repo", {{"root", root.string()},
                            {"name", root.filename().string()},
                            {"head", ""},
                            {"branch", ""},
                            {"detached", false}}},
                  {"session", {{"id", "live"},
                               {"name", "watching " + root.filename().string()},
                               {"baseline_generation", generation},
                               {"started_at_ms", 0}}},
                  {"generation", generation},
                  {"nodes", nodes},
                  {"edges", edges}};
        emit(json{{"type", "snapshot"}, {"snapshot", snap}});
    }

    // Two adapters in one stream (contract §6.2): the walk, and the manifest reader.
    // They are announced separately because the `provider` on an edge names one of them,
    // and the inspector should be able to say which.
    // `idle` with the baseline as last success: the walk is done and nothing is queued.
    // (`ready` is not a contract state; the parser drops the line and the inspector
    // never learns the adapter exists.)
    for (const char* adapter :
         {"filesystem", "npm", "python", "python-imports", "ts-imports", "cpp-imports"}) {
        emit(json{{"t_ms", 0}, {"type", "adapter.status"}, {"generation", generation},
                  {"adapter", adapter}, {"state", "idle"}, {"queue_depth", 0},
                  {"last_success_generation", generation}, {"message", ""}});
    }

    std::fprintf(stderr,
                 "rgv-watch: %zu entries, %zu packages, %zu python packages, %zu build "
                 "targets, %zu imports, %zu shared symbols under %s\n",
                 tree.entries.size(), packages.size(), pypkgs.size(), targets.own.names.size(),
                 imports.edges.size(), symbols.nodes.size(), root.c_str());

    Watcher watcher(root, ignores);
    watcher.add_recursive(root);

    while (!g_stop) {
        struct pollfd pfd { watcher.fd(), POLLIN, 0 };
        const int     n = ::poll(&pfd, 1, 250);
        if (n <= 0) continue;
        if (!watcher.drain()) continue;

        // Coalesce: an editor writing a file produces a burst, and one save should be
        // one generation, not eight. The debounce is what keeps NFR-01 honest.
        for (int quiet = 0; quiet < 3 && !g_stop;) {
            struct pollfd again { watcher.fd(), POLLIN, 0 };
            if (::poll(&again, 1, 40) > 0 && watcher.drain()) quiet = 0;
            else ++quiet;
        }

        // Reassigned in place, so the watcher's reference stays good: a `.gitignore`
        // edit is a change like any other and takes effect on the walk it caused.
        ignores = ignores_under(root);
        Tree now;
        walk(root, root, now, ignores);

        json added   = json::array();
        json removed = json::array();
        std::vector<std::pair<std::string, bool>> fresh;

        for (const auto& [rel, is_dir] : now.entries) {
            if (!tree.entries.count(rel)) fresh.emplace_back(rel, is_dir);
        }
        for (const auto& [rel, is_dir] : tree.entries) {
            if (!now.entries.count(rel)) {
                removed.push_back(is_dir ? dir_id(rel) : file_id(rel));
                if (!is_dir) {
                    imports.forget(rel);
                    symbols.forget(rel);
                    targets.forget(rel);
                    changed.erase(rel);
                    mtimes.erase(rel);
                }
            }
        }
        // A directory became a package, or stopped being one. The package node replaces
        // the directory node and everything directly inside re-parents -- reported as
        // replacements, so the frontend moves what changed and leaves the rest alone.
        json updated = json::array();
        if (!fresh.empty() || !removed.empty()) {
            reroot(now);
            const auto next_py = drop_distribution_twins(detect_python_packages(now, imports.roots),
                                                         packages, manifest_dirs, dist_module_nodes);
            if (next_py != pypkgs) {
                const PackageDirs next_dirs = merge_dirs(next_py);
                for (const auto& p : next_py) {
                    if (pkg_dirs.count(p.rel)) continue;   // already a package
                    added.push_back(pypkg_node(p, next_dirs));
                    if (tree.entries.count(p.rel)) removed.push_back(dir_id(p.rel));
                }
                for (const auto& p : pypkgs) {
                    if (next_dirs.count(p.rel)) continue;
                    removed.push_back(pypkg_id(p.rel));
                    if (now.entries.count(p.rel)) added.push_back(node_for(root, p.rel, true, next_dirs, targets.own));
                }
                for (const auto& [rel, is_dir] : now.entries) {
                    if (!tree.entries.count(rel) || pkg_dirs.count(rel) || next_dirs.count(rel)) continue;
                    if (parent_id_for(rel, pkg_dirs) != parent_id_for(rel, next_dirs)) {
                        updated.push_back(node_for(root, rel, is_dir, next_dirs, targets.own));
                    }
                }
                for (const auto& p : next_py) {
                    if (!pkg_dirs.count(p.rel)) continue;
                    if (parent_id_for(p.rel, pkg_dirs) != parent_id_for(p.rel, next_dirs)) {
                        updated.push_back(pypkg_node(p, next_dirs));
                    }
                }
                pypkgs   = next_py;
                pkg_dirs = next_dirs;
            }
        }

        bool reparsed = false;
        for (const auto& [rel, is_dir] : fresh) {
            if (is_dir) continue;
            if (is_source(rel)) { parse_source(rel); reparsed = true; }
            if (is_cmake(rel)) { targets.parse(rel, read_text(root / rel)); reparsed = true; }
        }

        // Content changes do not show up in the tree listing, so mtimes decide. A file
        // that just appeared is already an added node; saying it also "changed" would
        // double-count one event.
        std::vector<std::string> modified;
        for (const auto& [rel, is_dir] : now.entries) {
            if (is_dir) continue;
            const long long stamp = stamp_of(rel);
            auto            it    = mtimes.find(rel);
            const bool      moved = it != mtimes.end() && it->second != stamp;
            mtimes[rel]           = stamp;
            if (!moved) continue;
            modified.push_back(rel);
            if (is_source(rel)) { parse_source(rel); reparsed = true; }
            if (is_cmake(rel)) { targets.parse(rel, read_text(root / rel)); reparsed = true; }
        }

        // What a build target owns moves when a build file changes and when an include
        // does -- a header a second target starts including changes hands -- so
        // ownership is re-derived from the whole tree and reported as the difference,
        // the same discipline as the package nodes above.
        if (!fresh.empty() || !removed.empty() || reparsed) {
            auto next_own = targets.ownership(now, imports.cpp_graph(now));
            if (next_own.names != targets.own.names || next_own.parent != targets.own.parent ||
                next_own.home != targets.own.home) {
                for (const auto& name : targets.own.names) {
                    if (!TargetIndex::has(next_own, name)) removed.push_back(target_id(name));
                }
                for (const auto& name : next_own.names) {
                    const bool fresh_target = !TargetIndex::has(targets.own, name);
                    if (!fresh_target && next_own.home.at(name) == targets.own.home.at(name)) continue;
                    json n = targets.node_json_for(next_own, name, pkg_dirs);
                    (fresh_target ? added : updated).push_back(std::move(n));
                }
                // Everything whose containment parent moved with it. A node that has
                // only just arrived is already an `added_nodes` entry, and saying it
                // also changed would be the same node twice in one event.
                std::set<std::string> touched;
                for (const auto& [rel, name] : targets.own.parent) touched.insert(rel);
                for (const auto& [rel, name] : next_own.parent) touched.insert(rel);
                for (const auto& rel : touched) {
                    auto before = targets.own.parent.find(rel);
                    auto after  = next_own.parent.find(rel);
                    const bool same =
                        (before == targets.own.parent.end()) == (after == next_own.parent.end()) &&
                        (before == targets.own.parent.end() || before->second == after->second);
                    auto entry = now.entries.find(rel);
                    if (same || entry == now.entries.end() || !tree.entries.count(rel)) continue;
                    updated.push_back(node_for(root, rel, entry->second, pkg_dirs, next_own));
                }
                targets.own = std::move(next_own);
            }
        }

        for (const auto& [rel, is_dir] : fresh) {
            json n = node_for(root, rel, is_dir, pkg_dirs, targets.own);
            if (!n.is_null()) added.push_back(std::move(n));
        }

        const bool structural = !added.empty() || !removed.empty() || !updated.empty();
        json       added_edges   = json::array();
        json       removed_edges = json::array();
        json       updated_edges = json::array();
        if (structural || reparsed) {
            imports.resolve(now, generation + 1, owner_of, added_edges, removed_edges, updated_edges);
            targets.resolve_owns(targets.own, generation + 1, added_edges, removed_edges);
            symbols.resolve(imports, now, generation + 1, added, removed, updated, added_edges,
                            removed_edges, updated_edges);
        }
        const bool rewired = !added_edges.empty() || !removed_edges.empty() || !updated_edges.empty() ||
                             !added.empty() || !removed.empty() || !updated.empty();

        if (structural || rewired) {
            ++generation;
            emit(json{{"t_ms", 0}, {"type", "graph.updated"}, {"generation", generation},
                      {"added_nodes", added}, {"removed_nodes", removed}, {"updated_nodes", updated},
                      {"added_edges", added_edges}, {"removed_edges", removed_edges},
                      {"updated_edges", updated_edges},
                      {"note", rewired ? "python imports" : "filesystem"}});
        }

        // Content changes are reported per file, which is what makes a save show up as
        // "the agent touched this" rather than as a silent graph edit.
        for (const auto& rel : modified) {
            changed.insert(rel);
            ++generation;
            emit(json{{"t_ms", 0}, {"type", "file.changed"}, {"generation", generation},
                      {"path", rel}, {"node_id", file_id(rel)},
                      {"change", "modified"},
                      // The import scan is synchronous and already done by the time
                      // this line is written, and there is no semantic tier behind it,
                      // so nothing is outstanding for this file.
                      {"processing", "settled"}});
        }

        // The blast radius is the provider's to compute (contract §6.4), and it moves
        // whenever the seeds or the edges do.
        if ((!modified.empty() || rewired) && !changed.empty()) {
            std::vector<std::string> file_seeds;
            std::vector<std::string> pkg_seeds;
            std::vector<std::string> tgt_seeds;
            auto seed = [](std::vector<std::string>& seeds, const std::string& id) {
                if (!id.empty() && std::find(seeds.begin(), seeds.end(), id) == seeds.end()) {
                    seeds.push_back(id);
                }
            };
            for (const auto& rel : changed) {
                file_seeds.push_back(file_id(rel));
                seed(pkg_seeds, owning_package(rel, pkg_dirs));
                seed(tgt_seeds, targets.owner_id(rel));
            }
            emit(impact_json("file", {"imports"}, generation, baseline, file_seeds,
                             rgv::watch::reverse_reach(imports.graph_edges(), file_seeds,
                                                       baseline, 8)));
            const auto sym_seeds = symbols.seeds_in(changed);
            if (!sym_seeds.empty()) {
                emit(impact_json("symbol", {"references", "calls"}, generation, baseline, sym_seeds,
                                 rgv::watch::reverse_reach(symbols.graph_edges(), sym_seeds, baseline, 8)));
            }
            if (!pkg_seeds.empty()) {
                auto all = pkg_edges;
                for (auto& e : imports.dep_graph_edges()) all.push_back(std::move(e));
                emit(impact_json("package", {"depends_on"}, generation, baseline, pkg_seeds,
                                 rgv::watch::reverse_reach(all, pkg_seeds, baseline, 8)));
            }
            // The same question one level up for a language with no packages: change a
            // header, and the targets that have to be rebuilt light up.
            if (!tgt_seeds.empty()) {
                emit(impact_json("build_target", {"depends_on"}, generation, baseline, tgt_seeds,
                                 rgv::watch::reverse_reach(imports.dep_graph_edges(), tgt_seeds,
                                                           baseline, 8)));
            }
        }

        tree = std::move(now);
    }

    std::fprintf(stderr, "rgv-watch: stopping\n");
    return 0;
}
