// rgv-watch -- the filesystem provider.
//
// Walks a checkout, emits it as a contract snapshot, then watches it and emits deltas.
// It speaks contract §6: newline-delimited JSON on stdout, one message per line, the
// first a `snapshot` and the rest events. Diagnostics go to stderr and are never
// protocol.
//
// It knows nothing about any language. That is deliberate and is why this is the first
// provider: it is the whole live path -- watcher, generation, delta, transport -- with
// no commitment to an ecosystem, and every line of it is reused by whatever extractor
// comes next. Dependency edges are a different provider's job.
//
// Linux-only (inotify). The transport and the walk are portable; only `Watcher` is not.

#include "Packages.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void                       on_signal(int) { g_stop = 1; }

// Directories that are never interesting and are expensive to watch. A build tree can
// out-produce the graph by orders of magnitude and drown the real signal.
const std::unordered_set<std::string>& skipped() {
    static const std::unordered_set<std::string> s{
        ".git",   ".hg",     ".svn",         "node_modules", "build",  "dist",
        "target", ".venv",   "venv",         "__pycache__",  ".cache", ".mypy_cache",
        ".idea",  ".vscode", ".pytest_cache", ".ruff_cache", ".tox",   ".next"};
    return s;
}

bool ignored(const std::string& name) {
    return name.empty() || skipped().count(name) > 0;
}

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

void walk(const fs::path& root, const fs::path& dir, Tree& tree) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (ignored(name)) continue;
        if (name.size() > 1 && name[0] == '.') continue;   // dotfiles stay out of the way

        const std::string rel = rel_of(root, e.path());
        const bool        is_dir = e.is_directory(ec);
        tree.entries[rel]        = is_dir;
        if (is_dir) walk(root, e.path(), tree);
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
              const PackageDirs& pkg_dirs) {
    const fs::path    p         = root / rel;
    const std::string parent_id = parent_id_for(rel, pkg_dirs);

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
                   long generation) {
    std::map<std::string, const rgv::watch::Package*> by_name;
    for (const auto& p : pkgs) by_name[rgv::watch::normalize(p.name)] = &p;

    for (const auto& p : pkgs) {
        json n = node_json(p.id, "package", p.name, p.rel,
                           p.rel.empty() ? "repo:root" : dir_id(fs::path(p.rel).parent_path()
                                                                   .generic_string()),
                           p.provider == "python" ? "python" : "");
        json attrs{{"manifest", p.manifest}};
        if (!p.version.empty()) attrs["version"] = p.version;
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
            edges.push_back(std::move(e));
        }
    }
}

// -- inotify ------------------------------------------------------------------
//
// Recursive watching is not a thing inotify offers, so every directory gets its own
// watch and new directories are added as they appear.
class Watcher {
public:
    explicit Watcher(const fs::path& root) : root_(root) {
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
            const std::string name = e.path().filename().string();
            if (ignored(name) || (name.size() > 1 && name[0] == '.')) continue;
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

    Tree tree;
    walk(root, root, tree);

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

    std::vector<rgv::watch::Package> packages;
    PackageDirs                 pkg_dirs;

    // -- baseline
    {
        json nodes = json::array();
        json edges = json::array();
        nodes.push_back(node_json("repo:root", "repository", root.filename().string(), "", {}, {}));

        packages = rgv::watch::scan_packages(root, tree.entries);
        for (const auto& p : packages) pkg_dirs[p.rel] = p.id;

        for (const auto& [rel, is_dir] : tree.entries) {
            json n = node_for(root, rel, is_dir, pkg_dirs);
            if (!n.is_null()) nodes.push_back(std::move(n));
        }
        emit_packages(packages, nodes, edges, generation);

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
    for (const char* adapter : {"filesystem", "npm", "python"}) {
        emit(json{{"t_ms", 0}, {"type", "adapter.status"}, {"generation", generation},
                  {"adapter", adapter}, {"state", "ready"}, {"queue_depth", 0}});
    }

    std::fprintf(stderr, "rgv-watch: %zu entries, %zu packages under %s\n",
                 tree.entries.size(), packages.size(), root.c_str());

    Watcher watcher(root);
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

        Tree now;
        walk(root, root, now);

        json added   = json::array();
        json removed = json::array();
        std::vector<std::pair<std::string, bool>> fresh;

        for (const auto& [rel, is_dir] : now.entries) {
            if (!tree.entries.count(rel)) fresh.emplace_back(rel, is_dir);
        }
        for (const auto& [rel, is_dir] : tree.entries) {
            if (!now.entries.count(rel)) removed.push_back(is_dir ? dir_id(rel) : file_id(rel));
        }
        for (const auto& [rel, is_dir] : fresh) {
            json n = node_for(root, rel, is_dir, pkg_dirs);
            if (!n.is_null()) added.push_back(std::move(n));
        }

        const bool structural = !added.empty() || !removed.empty();
        if (structural) {
            ++generation;
            emit(json{{"t_ms", 0}, {"type", "graph.updated"}, {"generation", generation},
                      {"added_nodes", added}, {"removed_nodes", removed},
                      {"note", "filesystem"}});
        }

        // Content changes are reported per file, which is what makes a save show up as
        // "the agent touched this" rather than as a silent graph edit.
        for (const auto& [rel, is_dir] : now.entries) {
            if (is_dir) continue;
            const long long stamp = stamp_of(rel);
            auto            it    = mtimes.find(rel);
            const bool      known = it != mtimes.end();
            const bool      moved = known && it->second != stamp;
            mtimes[rel]           = stamp;
            // A file that just appeared is already reported as an added node; saying it
            // also "changed" would double-count one event.
            if (!moved) continue;

            ++generation;
            emit(json{{"t_ms", 0}, {"type", "file.changed"}, {"generation", generation},
                      {"path", rel}, {"node_id", file_id(rel)},
                      {"change", "modified"},
                      // Nothing is outstanding: this provider does no semantic work, so
                      // there is no later tier for this file to be waiting on. A
                      // language extractor would report `pending` here instead.
                      {"processing", "settled"}});
        }

        tree = std::move(now);
    }

    std::fprintf(stderr, "rgv-watch: stopping\n");
    return 0;
}
