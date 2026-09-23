// The live transport (contract 6). These run a real child process, because the whole
#include <unistd.h>
// point of this class is what happens across a pipe: partial reads, a provider that
// exits, a line that arrives in two pieces.

#include "TestMain.h"

#include "rgv/live/LiveSource.h"
#include "rgv/model/GraphStore.h"
#include "rgv/sim/ImpactSim.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace rgv;

namespace {

// A provider is anything that writes the contract to stdout, so a shell script is a
// perfectly good one to test against.
std::vector<std::string> sh(const std::string& script) {
    return {"/bin/sh", "-c", script};
}

const char* kSnapshot =
    R"({"type":"snapshot","snapshot":{"schema":"rgv.snapshot/1","generation":100,)"
    R"("session":{"id":"t","baseline_generation":100},)"
    R"("nodes":[{"id":"repo:root","kind":"repository","name":"r","path":""},)"
    R"({"id":"file:a.py","kind":"file","name":"a.py","path":"a.py","parent":"repo:root"}]}})";

} // namespace

// A message must never be parsed before its terminator arrives, or a chunk boundary in
// the middle of a JSON object becomes a parse error and a lost event.
TEST(a_line_split_across_reads_is_held_until_it_is_complete) {
    std::string carry;

    auto first = live::take_lines(carry, R"({"type":"file.ch)");
    CHECK_EQ(first.size(), 0u);
    CHECK(!carry.empty());

    auto second = live::take_lines(carry, "anged\"}\n");
    CHECK_EQ(second.size(), 1u);
    CHECK_EQ(second[0], R"({"type":"file.changed"})");
    CHECK(carry.empty());
}

TEST(several_lines_in_one_read_all_arrive) {
    std::string carry;
    auto        lines = live::take_lines(carry, "a\nb\nc\n");
    CHECK_EQ(lines.size(), 3u);
    CHECK_EQ(lines[2], "c");
}

// Blank lines are framing, not messages. A provider that pads its output must not
// produce parse errors.
TEST(blank_lines_are_not_messages) {
    std::string carry;
    auto        lines = live::take_lines(carry, "a\n\n\nb\n");
    CHECK_EQ(lines.size(), 2u);
}

TEST(the_baseline_arrives_before_the_source_is_usable) {
    live::LiveSource src(sh(std::string("printf '%s\\n' '") + kSnapshot + "'; sleep 5"), 4000.0);
    CHECK_EQ(src.baseline().nodes.size(), 2u);
    CHECK_EQ(src.baseline().generation, Generation{100});
    CHECK(src.status().attached);
    CHECK(src.timeline() == nullptr);   // a live stream cannot be scrubbed
}

// A provider that never says anything must not hang the application.
TEST(a_silent_provider_fails_to_attach_rather_than_hanging) {
    bool threw = false;
    try {
        live::LiveSource src(sh("sleep 5"), 300.0);
    } catch (const live::SpawnError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST(a_missing_provider_reports_rather_than_crashing) {
    bool threw = false;
    try {
        live::LiveSource src({"rgv-no-such-provider-anywhere"}, 500.0);
    } catch (const live::SpawnError&) {
        threw = true;
    }
    CHECK(threw);
}

// The one that matters: events written after the baseline have to reach the sink.
TEST(events_after_the_baseline_reach_the_sink) {
    const std::string ev =
        R"({"t_ms":0,"type":"file.changed","generation":101,"path":"a.py",)"
        R"("node_id":"file:a.py","change":"modified","processing":"settled"})";

    live::LiveSource src(
        sh(std::string("printf '%s\\n' '") + kSnapshot + "'; sleep 0.3; printf '%s\\n' '" + ev +
           "'; sleep 5"),
        4000.0);

    GraphStore store;
    store.reset(src.baseline());

    int drained = 0;
    for (int i = 0; i < 400 && drained == 0; ++i) {
        drained = src.poll(0.01, store);
        if (drained == 0) ::usleep(10000);
    }

    CHECK_EQ(drained, 1);
    CHECK_EQ(store.changed_files().size(), 1u);
    CHECK_EQ(store.changed_files()[0].node_id, std::string("file:a.py"));
}

// A provider exiting is end-of-stream, not a crash. The graph it produced is kept.
TEST(a_provider_that_exits_ends_the_stream_without_taking_the_app_with_it) {
    live::LiveSource src(sh(std::string("printf '%s\\n' '") + kSnapshot + "'"), 4000.0);

    GraphStore store;
    for (int i = 0; i < 200 && !src.status().ended; ++i) {
        src.poll(0.01, store);
        ::usleep(5000);
    }
    CHECK(src.status().ended);
    CHECK_EQ(src.baseline().nodes.size(), 2u);
}

// One bad line must not end the session -- a provider is another program, and it will
// eventually write something malformed.
TEST(a_malformed_line_is_skipped_not_fatal) {
    const std::string ev =
        R"({"t_ms":0,"type":"file.changed","generation":101,"path":"a.py",)"
        R"("node_id":"file:a.py","change":"modified","processing":"settled"})";

    live::LiveSource src(
        sh(std::string("printf '%s\\n' '") + kSnapshot + "'; printf 'not json at all\\n'; " +
           "printf '%s\\n' '" + ev + "'; sleep 5"),
        4000.0);

    GraphStore store;
    store.reset(src.baseline());
    for (int i = 0; i < 400 && store.changed_files().empty(); ++i) {
        src.poll(0.01, store);
        ::usleep(10000);
    }
    CHECK_EQ(store.changed_files().size(), 1u);
}

// -- the real provider --------------------------------------------------------
//
// Everything above stubs the provider with a shell script, which tests the transport
// but not the thing most likely to break: whether rgv-watch and the parser agree on the
// vocabulary. They did not. The provider emitted `"processing":"complete"`, which is not
// a Processing value, so every file.changed was dropped as a bad line and the live view
// showed a graph that never updated -- with no error anywhere the user could see.

TEST(the_filesystem_provider_produces_a_graph_the_parser_accepts) {
    const std::string root = std::string(RGV_TEST_TMP) + "/provider-baseline";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/pkg/sub");
    { std::ofstream(root + "/pkg/a.py") << "x = 1\n"; }
    { std::ofstream(root + "/pkg/sub/b.py") << "y = 2\n"; }

    live::LiveSource src({RGV_WATCH_BIN, "--root", root}, 5000.0);

    const auto& base = src.baseline();
    CHECK(base.nodes.size() >= 5u);   // repo + pkg + sub + two files

    int files = 0, dirs = 0, repos = 0;
    for (const auto& n : base.nodes) {
        if (n.kind == NodeKind::File) ++files;
        if (n.kind == NodeKind::Directory) ++dirs;
        if (n.kind == NodeKind::Repository) ++repos;
    }
    CHECK_EQ(repos, 1);
    CHECK_EQ(files, 2);
    CHECK_EQ(dirs, 2);

    // Containment is a tree, and the frontend projects files to packages through it.
    for (const auto& n : base.nodes) {
        if (n.kind != NodeKind::Repository) CHECK(!n.parent.empty());
    }
}

// A save has to arrive as a change, through the real provider and the real parser.
TEST(a_file_saved_on_disk_arrives_as_a_change) {
    const std::string root = std::string(RGV_TEST_TMP) + "/provider-live";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    { std::ofstream(root + "/a.py") << "x = 1\n"; }

    live::LiveSource src({RGV_WATCH_BIN, "--root", root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());

    // Let the watcher settle before touching anything, or the write races the watch.
    for (int i = 0; i < 40; ++i) { src.poll(0.01, store); ::usleep(10000); }

    { std::ofstream(root + "/a.py", std::ios::app) << "y = 2\n"; }

    for (int i = 0; i < 500 && store.changed_files().empty(); ++i) {
        src.poll(0.01, store);
        ::usleep(10000);
    }
    CHECK_EQ(store.changed_files().size(), 1u);
    CHECK_EQ(store.changed_files()[0].path, std::string("a.py"));
}

// A file appearing is a graph delta, not a change to an existing node.
TEST(a_file_created_on_disk_arrives_as_a_new_node) {
    const std::string root = std::string(RGV_TEST_TMP) + "/provider-add";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    { std::ofstream(root + "/a.py") << "x = 1\n"; }

    live::LiveSource src({RGV_WATCH_BIN, "--root", root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    const std::size_t before = store.nodes().size();

    for (int i = 0; i < 40; ++i) { src.poll(0.01, store); ::usleep(10000); }
    { std::ofstream(root + "/b.py") << "z = 3\n"; }

    for (int i = 0; i < 500 && store.nodes().size() == before; ++i) {
        src.poll(0.01, store);
        ::usleep(10000);
    }
    CHECK_EQ(store.nodes().size(), before + 1);
    CHECK(store.node("file:b.py") != nullptr);
}

// `./build/bin/rgv --watch .` failed with "could not start 'rgv-watch': No such file or
// directory" unless the build directory happened to be on PATH. Every test and every
// manual check had put it there, so the one command the README documents was the one
// path nothing exercised.
TEST(a_bare_provider_name_resolves_next_to_the_frontend) {
    // The test binary lives beside rgv-watch, exactly as the app does.
    const std::string resolved = live::resolve_provider("rgv-watch");
    CHECK(resolved != "rgv-watch");
    CHECK(std::filesystem::exists(resolved));

    // Attaching by bare name must work with no PATH help at all.
    ::unsetenv("PATH");
    const std::string root = std::string(RGV_TEST_TMP) + "/provider-resolve";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    { std::ofstream(root + "/a.py") << "x = 1\n"; }

    live::LiveSource src({live::resolve_provider("rgv-watch"), "--root", root}, 5000.0);
    CHECK(src.baseline().nodes.size() >= 2u);
}

// A path is a path. Resolution must not rewrite what the user spelled out.
TEST(an_explicit_provider_path_is_used_as_given) {
    CHECK_EQ(live::resolve_provider("./build/bin/rgv-watch"), std::string("./build/bin/rgv-watch"));
    CHECK_EQ(live::resolve_provider(RGV_WATCH_BIN), std::string(RGV_WATCH_BIN));
}

// A package node REPLACES the directory node at its path rather than sitting beside it,
// and everything inside reparents onto it. That is what makes "which package owns this
// file" a walk up the containment tree instead of a path-prefix search (FR-11), and two
// nodes for one path would put the same directory on screen twice.
TEST(a_package_replaces_the_directory_it_occupies) {
    const std::string root = std::string(RGV_TEST_TMP) + "/provider-packages";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/libs/core/src");
    std::filesystem::create_directories(root + "/services/api/src");
    { std::ofstream(root + "/libs/core/pyproject.toml")
          << "[project]\nname = \"acme-core\"\ndependencies = [\"structlog\"]\n"; }
    { std::ofstream(root + "/services/api/pyproject.toml")
          << "[project]\nname = \"acme-api\"\ndependencies = [\"acme-core\", \"fastapi\"]\n"; }
    { std::ofstream(root + "/libs/core/src/core.py") << "x = 1\n"; }

    live::LiveSource src({live::resolve_provider("rgv-watch"), "--root", root}, 5000.0);
    const auto&      base = src.baseline();

    auto node = [&](const std::string& id) -> const Node* {
        for (const auto& n : base.nodes) {
            if (n.id == id) return &n;
        }
        return nullptr;
    };

    CHECK(node("pkg:acme-core") != nullptr);
    CHECK(node("dir:libs/core") == nullptr);          // not both
    CHECK(node("pkg:acme-core")->kind == NodeKind::Package);
    CHECK_EQ(node("pkg:acme-core")->path, std::string("libs/core"));

    // Children route through the package.
    CHECK_EQ(node("dir:libs/core/src")->parent, std::string("pkg:acme-core"));
    CHECK_EQ(node("file:libs/core/pyproject.toml")->parent, std::string("pkg:acme-core"));

    // The dependency the Architecture view exists to draw, dependent -> dependency.
    int internal = 0;
    for (const auto& e : base.edges) {
        if (e.kind != EdgeKind::DependsOn) continue;
        if (e.from == "pkg:acme-api" && e.to == "pkg:acme-core") {
            ++internal;
            CHECK(e.confidence == Confidence::Exact);
            // Evidence is what the provenance inspector shows. An edge nobody can trace
            // back to a line in a file is an assertion taken on faith.
            CHECK(e.evidence.has_value());
            CHECK_EQ(e.evidence->artifact, std::string("services/api/pyproject.toml"));
            CHECK(e.evidence->line > 0);
        }
    }
    CHECK_EQ(internal, 1);

    // A third-party dependency is context, not a node in the repository.
    CHECK(node("ext:fastapi") != nullptr);
    CHECK(node("ext:fastapi")->kind == NodeKind::ExternalPackage);
    CHECK(node("ext:fastapi")->parent.empty());
}

// -- the python extractor, end to end ----------------------------------------
//
// Through the real provider and the real parser again, because the vocabulary is the
// thing most likely to drift: an edge kind, a confidence, an impact field.

namespace {

struct PyRepo {
    std::string root;
    explicit PyRepo(const char* name) : root(std::string(RGV_TEST_TMP) + "/" + name) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    void write(const std::string& rel, const std::string& text) const {
        const auto p = std::filesystem::path(root) / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p) << text;
    }
};

const Edge* import_edge(const GraphStore& store, const std::string& from, const std::string& to) {
    for (const auto& [id, e] : store.edges()) {
        if (e.kind == EdgeKind::Imports && e.from == "file:" + from && e.to == "file:" + to &&
            e.active()) {
            return &e;
        }
    }
    return nullptr;
}

void settle(live::LiveSource& src, GraphStore& store) {
    for (int i = 0; i < 40; ++i) { src.poll(0.01, store); ::usleep(10000); }
}

template <class Pred>
bool wait_for(live::LiveSource& src, GraphStore& store, Pred done) {
    for (int i = 0; i < 500; ++i) {
        src.poll(0.01, store);
        if (done()) return true;
        ::usleep(10000);
    }
    return done();
}

} // namespace

TEST(the_python_provider_emits_import_edges_the_parser_accepts) {
    PyRepo r("provider-py-baseline");
    r.write("pyproject.toml", "[project]\nname = \"demo\"\n");
    r.write("demo/__init__.py", "");
    r.write("demo/a.py", "import os\nfrom demo import b\n");
    r.write("demo/b.py", "x = 1\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());

    const Edge* e = import_edge(store, "demo/a.py", "demo/b.py");
    CHECK(e != nullptr);
    CHECK_EQ(e->provider, std::string("python-imports"));
    CHECK(e->confidence == Confidence::Exact);
    CHECK(e->evidence.has_value());
    CHECK_EQ(e->evidence->artifact, std::string("demo/a.py"));
    CHECK_EQ(e->evidence->line, 2);
    CHECK_EQ(e->evidence->snippet, std::string("from demo import b"));

    // `import os` resolves to nothing in the repository and must not become an edge.
    int imports = 0;
    for (const auto& [id, edge] : store.edges()) {
        if (edge.kind == EdgeKind::Imports) ++imports;
    }
    CHECK_EQ(imports, 1);

    // The adapter behind the edge is announced, so the inspector can name it.
    settle(src, store);
    bool announced = false;
    for (const auto& a : store.adapters()) {
        if (a.name == "python-imports") announced = true;
    }
    CHECK(announced);
}

// The product: save b.py, and a.py lights up because it imports b. The result must
// agree with the traversal the frontend itself would perform, which is what
// `rgv-replay --check` demands of a fixture.
TEST(saving_a_python_file_lights_up_the_files_that_import_it) {
    PyRepo r("provider-py-impact");
    r.write("pyproject.toml", "[project]\nname = \"demo\"\n");
    r.write("demo/__init__.py", "");
    r.write("demo/a.py", "from demo import b\n");
    r.write("demo/b.py", "x = 1\n");
    r.write("demo/c.py", "y = 2\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    settle(src, store);

    { std::ofstream(r.root + "/demo/b.py", std::ios::app) << "z = 3\n"; }

    CHECK(wait_for(src, store, [&] { return store.impact(Level::File) != nullptr; }));
    const ImpactResult* file = store.impact(Level::File);
    CHECK_EQ(file->seed_nodes.size(), 1u);
    CHECK_EQ(file->seed_nodes[0], std::string("file:demo/b.py"));

    const ImpactedNode* a = nullptr;
    for (const auto& n : file->impacted_nodes) {
        if (n.node_id == "file:demo/a.py") a = &n;
        CHECK(n.node_id != "file:demo/c.py");   // c imports nothing
    }
    CHECK(a != nullptr);
    CHECK_EQ(a->min_distance, 1);
    CHECK(a->direct);
    CHECK_EQ(a->paths.size(), 1u);
    CHECK_EQ(a->paths[0].edges.size(), 1u);
    CHECK(store.edge(a->paths[0].edges[0]) != nullptr);

    // Same answer as the frontend's own reverse closure over the same store.
    const auto mine = sim::compute(store, sim::seeds_for_level(store, Level::File), Level::File,
                                   file->filters);
    std::set<std::string> theirs, ours;
    for (const auto& n : file->impacted_nodes) theirs.insert(n.node_id);
    for (const auto& n : mine.impacted_nodes) ours.insert(n.node_id);
    CHECK(theirs == ours);

    // The package level is seeded by the package that owns the changed file.
    CHECK(wait_for(src, store, [&] { return store.impact(Level::Package) != nullptr; }));
    const ImpactResult* pkg = store.impact(Level::Package);
    CHECK_EQ(pkg->seed_nodes.size(), 1u);
    CHECK_EQ(pkg->seed_nodes[0], std::string("pkg:demo"));
}

// An import rewritten is an edge removed and an edge added, not a rebuilt graph.
TEST(rewriting_an_import_moves_the_edge) {
    PyRepo r("provider-py-rewire");
    r.write("demo/__init__.py", "");
    r.write("demo/a.py", "from demo import b\n");
    r.write("demo/b.py", "x = 1\n");
    r.write("demo/c.py", "y = 2\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    settle(src, store);
    CHECK(import_edge(store, "demo/a.py", "demo/b.py") != nullptr);
    const std::size_t nodes_before = store.nodes().size();

    r.write("demo/a.py", "from demo import c\n");

    CHECK(wait_for(src, store, [&] { return import_edge(store, "demo/a.py", "demo/c.py") != nullptr; }));
    CHECK(import_edge(store, "demo/a.py", "demo/b.py") == nullptr);
    CHECK_EQ(store.nodes().size(), nodes_before);

    // a.py now depends on c: the blast radius of c includes a, of b does not.
    CHECK(wait_for(src, store, [&] {
        const auto* f = store.impact(Level::File);
        return f && f->seed_nodes.size() == 1 && f->seed_nodes[0] == "file:demo/a.py";
    }));
}

// A file that appears with imports arrives with its edges, and a deleted target takes
// the edges that pointed at it with it.
TEST(a_new_python_file_arrives_with_its_edges_and_a_deleted_one_takes_them) {
    PyRepo r("provider-py-addrm");
    r.write("demo/__init__.py", "");
    r.write("demo/b.py", "x = 1\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    settle(src, store);

    r.write("demo/a.py", "from demo import b\n");
    CHECK(wait_for(src, store, [&] { return import_edge(store, "demo/a.py", "demo/b.py") != nullptr; }));

    std::filesystem::remove(r.root + "/demo/b.py");
    CHECK(wait_for(src, store, [&] { return store.node("file:demo/b.py") == nullptr; }));
    CHECK(import_edge(store, "demo/a.py", "demo/b.py") == nullptr);
}

// -- python packages as architecture -----------------------------------------
//
// One pyproject.toml is one distribution, but the architecture of the code is the
// Python packages inside it and how they import each other. A single box is not an
// architecture view.

TEST(a_single_distribution_shows_its_python_packages_as_architecture) {
    PyRepo r("provider-py-arch");
    r.write("pyproject.toml", "[project]\nname = \"demo\"\n");
    r.write("src/demo/__init__.py", "");
    r.write("src/demo/app.py", "from demo.api import routes\n");
    r.write("src/demo/api/__init__.py", "");
    r.write("src/demo/api/routes.py", "from demo.core.db import connect\n");
    r.write("src/demo/core/__init__.py", "");
    r.write("src/demo/core/db.py", "def connect(): pass\n");
    r.write("tests/test_db.py", "from demo.core import db\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    settle(src, store);

    // The distribution and its top-level package share the name `demo`, so they are one
    // node. Two boxes with the same label is not an architecture, and the duplicate
    // doubles the edges between the packages underneath it.
    int named_demo = 0;
    for (const auto& [id, n] : store.nodes()) {
        if (n.kind == NodeKind::Package && n.name == "demo") ++named_demo;
    }
    CHECK_EQ(named_demo, 1);
    CHECK(store.node("pypkg:src/demo") == nullptr);

    const Node* demo = store.node("pkg:demo");
    const Node* api  = store.node("pypkg:src/demo/api");
    const Node* core = store.node("pypkg:src/demo/core");
    CHECK(demo != nullptr && api != nullptr && core != nullptr);
    CHECK(api->kind == NodeKind::Package);
    CHECK_EQ(api->name, std::string("demo.api"));
    CHECK(store.node("dir:src/demo/api") == nullptr);   // the package replaces the directory

    // The file that IS each package, so a real `from demo import x` can be told apart
    // from a reach into some module that merely lives inside it.
    CHECK_EQ(demo->attrs.at("module_file"), std::string("src/demo/__init__.py"));
    CHECK_EQ(api->attrs.at("module_file"), std::string("src/demo/api/__init__.py"));

    // The distribution stands where its code stands. `pyproject.toml` is at the repo
    // root, but the package it declares is `src/demo`, and seating the node on the
    // manifest's directory instead makes it the repository wearing a package's name --
    // it swallows `tests/`, `docs/` and every other top-level directory, and every
    // import out of them is then attributed to the architecture.
    CHECK_EQ(demo->path, std::string("src/demo"));
    CHECK_EQ(demo->parent, std::string("dir:src"));

    // Files are owned by the innermost package, which is what FR-11 projects through.
    CHECK_EQ(store.ancestor_of_kind("file:src/demo/api/routes.py", NodeKind::Package),
             std::string("pypkg:src/demo/api"));
    // The modules sitting directly in the distribution's own directory belong to it.
    CHECK_EQ(store.ancestor_of_kind("file:src/demo/app.py", NodeKind::Package),
             std::string("pkg:demo"));
    // The test suite does not. It is outside the package directory, so no package owns
    // it and its imports are not the architecture's edges.
    CHECK_EQ(store.ancestor_of_kind("file:tests/test_db.py", NodeKind::Package),
             std::string(""));

    // The edges between them, aggregated from the imports that cross the boundary.
    auto dep = [&](const std::string& from, const std::string& to) -> const Edge* {
        for (const auto& [id, e] : store.edges()) {
            if (e.kind == EdgeKind::DependsOn && e.from == from && e.to == to && e.active()) return &e;
        }
        return nullptr;
    };
    const Edge* api_core = dep("pypkg:src/demo/api", "pypkg:src/demo/core");
    CHECK(api_core != nullptr);
    CHECK_EQ(api_core->provider, std::string("python-imports"));
    CHECK(api_core->confidence == Confidence::Exact);
    CHECK_EQ(api_core->evidence->artifact, std::string("src/demo/api/routes.py"));
    CHECK(dep("pkg:demo", "pypkg:src/demo/api") != nullptr);   // app.py imports it
    CHECK(dep("pypkg:src/demo/core", "pypkg:src/demo/api") == nullptr);
    // `tests/test_db.py` is the only thing that imports `core` from outside, and it is
    // not in the package. Attributing it to the distribution is what turns a layered
    // graph into a hub -- and, where a test imports something above it, into a cycle.
    CHECK(dep("pkg:demo", "pypkg:src/demo/core") == nullptr);

    // Change db.py: api is directly impacted, the distribution too, and the answer
    // agrees with the frontend's own traversal.
    { std::ofstream(r.root + "/src/demo/core/db.py", std::ios::app) << "# more\n"; }
    CHECK(wait_for(src, store, [&] { return store.impact(Level::Package) != nullptr; }));
    const ImpactResult* pkg = store.impact(Level::Package);
    CHECK_EQ(pkg->seed_nodes.size(), 1u);
    CHECK_EQ(pkg->seed_nodes[0], std::string("pypkg:src/demo/core"));
    std::map<std::string, int> dist;
    for (const auto& n : pkg->impacted_nodes) dist[n.node_id] = n.min_distance;
    CHECK_EQ(dist["pypkg:src/demo/api"], 1);
    // The distribution reaches `core` through `api` now, not directly through a test.
    CHECK_EQ(dist["pkg:demo"], 2);

    const auto mine = sim::compute(store, sim::seeds_for_level(store, Level::Package),
                                   Level::Package, pkg->filters);
    std::set<std::string> theirs, ours;
    for (const auto& n : pkg->impacted_nodes) theirs.insert(n.node_id);
    for (const auto& n : mine.impacted_nodes) ours.insert(n.node_id);
    CHECK(theirs == ours);
}

// Dropping an `__init__.py` into a directory makes it a package. The directory node
// gives way to a package node and what it held re-parents, live, without a rebuild.
TEST(a_directory_becomes_a_package_when_an_init_appears) {
    PyRepo r("provider-py-newpkg");
    r.write("app/__init__.py", "");
    r.write("app/main.py", "from app.util import helpers\n");
    r.write("app/util/helpers.py", "x = 1\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    settle(src, store);
    CHECK(store.node("dir:app/util") != nullptr);
    CHECK_EQ(store.node("file:app/util/helpers.py")->parent, std::string("dir:app/util"));

    r.write("app/util/__init__.py", "");
    CHECK(wait_for(src, store, [&] { return store.node("pypkg:app/util") != nullptr; }));
    CHECK(store.node("dir:app/util") == nullptr);
    CHECK_EQ(store.node("pypkg:app/util")->name, std::string("app.util"));
    CHECK_EQ(store.node("pypkg:app/util")->parent, std::string("pypkg:app"));
    CHECK_EQ(store.node("file:app/util/helpers.py")->parent, std::string("pypkg:app/util"));
    CHECK_EQ(store.node("file:app/util/__init__.py")->parent, std::string("pypkg:app/util"));

    // And the import from main.py is now an edge between packages.
    CHECK(wait_for(src, store, [&] {
        for (const auto& [id, e] : store.edges()) {
            if (e.kind == EdgeKind::DependsOn && e.from == "pypkg:app" && e.to == "pypkg:app/util") return true;
        }
        return false;
    }));
}

// -- symbols, end to end -------------------------------------------------------
//
// A component class defined in one file, written by one system and read by another.
// The symbol arrives as a node under its file, the two systems attach to it with a
// `calls` and a `references` edge, and changing the file lights both systems up.

TEST(symbols_and_their_readers_and_writers_arrive_through_the_provider) {
    PyRepo r("provider-py-symbols");
    r.write("demo/__init__.py", "");
    r.write("demo/components/__init__.py", "from .car import CarState\n");
    r.write("demo/components/car.py", "class CarState:\n    pass\n");
    r.write("demo/systems/__init__.py", "");
    r.write("demo/systems/movement.py",
            "import esper\nfrom ..components import CarState\n\n"
            "def go(e):\n    esper.add_component(e, CarState())\n");
    r.write("demo/systems/render.py",
            "import esper\nfrom ..components.car import CarState\n\n"
            "def draw():\n    for e, s in esper.get_component(CarState):\n        pass\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    settle(src, store);

    const std::string sym = "sym:demo/components/car.py#CarState";
    const Node*       n   = store.node(sym);
    CHECK(n != nullptr);
    CHECK(n->kind == NodeKind::Symbol);
    CHECK_EQ(n->name, std::string("CarState"));
    CHECK_EQ(n->parent, std::string("file:demo/components/car.py"));
    CHECK_EQ(n->attrs.at("kind"), std::string("class"));
    // `go` and `draw` are used by nobody else and are not nodes.
    CHECK(store.node("sym:demo/systems/movement.py#go") == nullptr);

    auto edge_of = [&](EdgeKind kind, const std::string& from) -> const Edge* {
        for (const auto& [id, e] : store.edges()) {
            if (e.kind == kind && e.from == from && e.to == sym && e.active()) return &e;
        }
        return nullptr;
    };
    const Edge* write = edge_of(EdgeKind::Calls, "file:demo/systems/movement.py");
    CHECK(write != nullptr);
    CHECK_EQ(write->provider, std::string("python-imports"));
    CHECK_EQ(write->evidence->line, 5);
    CHECK_EQ(write->evidence->snippet, std::string("esper.add_component(e, CarState())"));
    const Edge* read = edge_of(EdgeKind::References, "file:demo/systems/render.py");
    CHECK(read != nullptr);
    CHECK(edge_of(EdgeKind::Calls, "file:demo/systems/render.py") == nullptr);

    // Change the component: both systems are in its blast radius, one hop each.
    { std::ofstream(r.root + "/demo/components/car.py", std::ios::app) << "    x = 1\n"; }
    CHECK(wait_for(src, store, [&] { return store.impact(Level::Symbol) != nullptr; }));
    const ImpactResult* res = store.impact(Level::Symbol);
    CHECK_EQ(res->seed_nodes.size(), 1u);
    CHECK_EQ(res->seed_nodes[0], sym);
    std::map<std::string, const ImpactedNode*> hit;
    for (const auto& in : res->impacted_nodes) hit[in.node_id] = &in;
    CHECK(hit.count("file:demo/systems/movement.py") == 1);
    CHECK(hit.count("file:demo/systems/render.py") == 1);
    CHECK_EQ(hit["file:demo/systems/render.py"]->min_distance, 1);
    CHECK_EQ(hit["file:demo/systems/render.py"]->paths.size(), 1u);
    CHECK(store.edge(hit["file:demo/systems/render.py"]->paths[0].edges[0]) != nullptr);

    // Stop reading it in render.py: that edge goes, the symbol stays for movement.
    r.write("demo/systems/render.py", "def draw():\n    pass\n");
    CHECK(wait_for(src, store, [&] { return edge_of(EdgeKind::References, "file:demo/systems/render.py") == nullptr; }));
    CHECK(store.node(sym) != nullptr);
    CHECK(edge_of(EdgeKind::Calls, "file:demo/systems/movement.py") != nullptr);
}

// -- typescript, end to end ------------------------------------------------------
//
// Resolution is the whole of the difference from Python, so this is where it is checked
// against the real provider: extensionless specifiers, the `.js` a TypeScript file
// writes when it means the `.ts` beside it, and a bare specifier that names a package
// in this repository rather than something in node_modules.

TEST(the_typescript_provider_resolves_node_style_imports) {
    PyRepo r("provider-ts");
    r.write("package.json", R"({"name":"acme","private":true,"workspaces":["packages/*"]})");
    r.write("packages/logger/package.json",
            R"({"name":"@acme/logger","version":"1.0.0","source":"src/index.ts"})");
    r.write("packages/logger/src/index.ts", "export function log(m: string) {}\n");
    r.write("packages/app/package.json",
            R"({"name":"@acme/app","dependencies":{"@acme/logger":"workspace:*"}})");
    r.write("packages/app/src/index.ts",
            "import { log } from '@acme/logger';\n"
            "import { util } from './util.js';\n"
            "import react from 'react';\n"
            "// import { fake } from './nope';\n"
            "export const go = () => log(String(util));\n");
    r.write("packages/app/src/util.ts", "export const util = 1;\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());

    // A bare specifier that names a package here, through the entry its manifest gives.
    const Edge* cross =
        import_edge(store, "packages/app/src/index.ts", "packages/logger/src/index.ts");
    CHECK(cross != nullptr);
    CHECK_EQ(cross->provider, std::string("ts-imports"));
    CHECK(cross->confidence == Confidence::Exact);

    // `./util.js` means the `.ts` beside it.
    CHECK(import_edge(store, "packages/app/src/index.ts", "packages/app/src/util.ts") != nullptr);

    // `react` is in node_modules and the commented-out line is not an import, so those
    // are the only two.
    int imports = 0;
    for (const auto& [id, e] : store.edges()) {
        if (e.kind == EdgeKind::Imports) ++imports;
    }
    CHECK_EQ(imports, 2);

    // And the packages underneath them.
    bool dep = false;
    for (const auto& [id, e] : store.edges()) {
        if (e.kind == EdgeKind::DependsOn && e.from == "pkg:@acme/app" &&
            e.to == "pkg:@acme/logger" && e.provider == "ts-imports") {
            dep = true;
        }
    }
    CHECK(dep);

    settle(src, store);
    bool announced = false;
    for (const auto& a : store.adapters()) {
        if (a.name == "ts-imports") announced = true;
    }
    CHECK(announced);
}


// -- the C++ extractor, end to end --------------------------------------------
//
// Through the real provider and the real parser again. What is worth checking here is
// the half that is not the import scanner: C++ has no packaging, so the unit above the
// file is the build target read out of CMakeLists.txt, and the containment it implies
// is what the architecture view folds an import along.

namespace {

// A library with its headers in an `include/` tree no build file mentions, and an
// application that links it. The shape nearly every C++ repository has.
PyRepo cpp_repo(const char* name) {
    PyRepo r(name);
    r.write("CMakeLists.txt",
            "add_library(core STATIC\n"
            "  lib/src/core.cpp\n"
            "  lib/src/util.cpp\n"
            ")\n"
            "add_executable(app app/main.cpp)\n");
    r.write("lib/include/lib/core.h", "#pragma once\n#include \"lib/util.h\"\nvoid core();\n");
    r.write("lib/include/lib/util.h", "#pragma once\nint util();\n");
    r.write("lib/src/core.cpp", "#include \"lib/core.h\"\n#include <vector>\nvoid core() {}\n");
    r.write("lib/src/util.cpp", "#include \"lib/util.h\"\nint util() { return 1; }\n");
    r.write("app/main.cpp",
            "#include <lib/core.h>\n"
            "#include <string>\n"
            "// #include <lib/util.h>\n"
            "int main() { core(); }\n");
    return r;
}

} // namespace

TEST(the_cpp_provider_resolves_includes_and_makes_targets_the_architecture) {
    PyRepo           r = cpp_repo("provider-cpp");
    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());

    // An angled include against the include directory, and a quoted one that is not
    // beside its file and falls through to the same place.
    const Edge* e = import_edge(store, "app/main.cpp", "lib/include/lib/core.h");
    CHECK(e != nullptr);
    CHECK_EQ(e->provider, std::string("cpp-imports"));
    CHECK(e->confidence == Confidence::Exact);
    CHECK(e->evidence.has_value());
    CHECK_EQ(e->evidence->artifact, std::string("app/main.cpp"));
    CHECK_EQ(e->evidence->line, 1);
    CHECK_EQ(e->evidence->snippet, std::string("#include <lib/core.h>"));

    // A header including a header, and a source including its own header: two nodes and
    // a real edge, not a pair to be folded away.
    CHECK(import_edge(store, "lib/include/lib/core.h", "lib/include/lib/util.h") != nullptr);
    CHECK(import_edge(store, "lib/src/core.cpp", "lib/include/lib/core.h") != nullptr);

    // `<vector>`, `<string>` and the commented-out line are not in the repository, so
    // those four are all there is.
    int imports = 0;
    for (const auto& [id, edge] : store.edges()) {
        if (edge.kind == EdgeKind::Imports) ++imports;
    }
    CHECK_EQ(imports, 4);

    const Node* core = store.node("tgt:core");
    const Node* app  = store.node("tgt:app");
    CHECK(core != nullptr && app != nullptr);
    CHECK(core->kind == NodeKind::BuildTarget);
    CHECK_EQ(core->attrs.at("type"), std::string("library"));
    CHECK_EQ(core->attrs.at("manifest"), std::string("CMakeLists.txt"));
    CHECK_EQ(core->path, std::string("lib"));   // where its code is: src/ and include/

    // The containment the architecture view needs. The headers are in no build file at
    // all, and the target owns them because its sources reach them -- without that the
    // only line a C++ repository could draw would end at the repository node.
    CHECK_EQ(store.ancestor_of_kind("file:lib/include/lib/util.h", NodeKind::BuildTarget),
             std::string("tgt:core"));
    CHECK_EQ(store.ancestor_of_kind("file:lib/src/core.cpp", NodeKind::BuildTarget),
             std::string("tgt:core"));
    CHECK_EQ(store.ancestor_of_kind("file:app/main.cpp", NodeKind::BuildTarget),
             std::string("tgt:app"));

    // And the edge between the targets, aggregated from the include that crosses.
    const Edge* dep = nullptr;
    for (const auto& [id, edge] : store.edges()) {
        if (edge.kind == EdgeKind::DependsOn && edge.from == "tgt:app" && edge.to == "tgt:core") {
            dep = &edge;
        }
    }
    CHECK(dep != nullptr);
    CHECK_EQ(dep->provider, std::string("cpp-imports"));
    CHECK_EQ(dep->evidence->artifact, std::string("app/main.cpp"));

    settle(src, store);
    bool announced = false;
    for (const auto& a : store.adapters()) {
        if (a.name == "cpp-imports") announced = true;
    }
    CHECK(announced);
}

// The product, for C++: touch a header and the targets that have to be rebuilt light up.
TEST(saving_a_header_lights_up_what_includes_it_and_the_targets_above) {
    PyRepo           r = cpp_repo("provider-cpp-impact");
    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());
    settle(src, store);

    { std::ofstream(r.root + "/lib/include/lib/util.h", std::ios::app) << "int more();\n"; }

    CHECK(wait_for(src, store, [&] { return store.impact(Level::File) != nullptr; }));
    const ImpactResult* file = store.impact(Level::File);
    CHECK_EQ(file->seed_nodes.size(), 1u);
    CHECK_EQ(file->seed_nodes[0], std::string("file:lib/include/lib/util.h"));

    std::map<std::string, int> dist;
    for (const auto& n : file->impacted_nodes) dist[n.node_id] = n.min_distance;
    CHECK_EQ(dist["file:lib/include/lib/core.h"], 1);
    CHECK_EQ(dist["file:lib/src/util.cpp"], 1);
    CHECK_EQ(dist["file:app/main.cpp"], 2);   // through core.h

    // Same answer as the frontend's own reverse closure over the same store.
    const auto mine = sim::compute(store, sim::seeds_for_level(store, Level::File), Level::File,
                                   file->filters);
    std::set<std::string> theirs, ours;
    for (const auto& n : file->impacted_nodes) theirs.insert(n.node_id);
    for (const auto& n : mine.impacted_nodes) ours.insert(n.node_id);
    CHECK(theirs == ours);

    // And one level up, where C++ has build targets instead of packages.
    CHECK(wait_for(src, store, [&] { return store.impact(Level::BuildTarget) != nullptr; }));
    const ImpactResult* tgt = store.impact(Level::BuildTarget);
    CHECK_EQ(tgt->seed_nodes.size(), 1u);
    CHECK_EQ(tgt->seed_nodes[0], std::string("tgt:core"));
    const ImpactedNode* hit = nullptr;
    for (const auto& n : tgt->impacted_nodes) {
        if (n.node_id == "tgt:app") hit = &n;
    }
    CHECK(hit != nullptr);
    CHECK_EQ(hit->min_distance, 1);
    CHECK(store.edge(hit->paths[0].edges[0]) != nullptr);
}

// Two build files naming one file is the multi-owner case the contract has `owns` for:
// one containment parent, and an edge for the claim that did not become the parent.
TEST(a_source_two_targets_build_has_one_parent_and_an_owns_edge) {
    PyRepo r("provider-cpp-owns");
    r.write("CMakeLists.txt",
            "add_executable(tool tools/dump.cpp shared/helper.cpp)\n"
            "add_executable(probe probe/main.cpp shared/helper.cpp)\n");
    r.write("shared/helper.cpp", "#include \"helper.h\"\n");
    r.write("shared/helper.h", "void help();\n");
    r.write("tools/dump.cpp", "int main() {}\n");
    r.write("probe/main.cpp", "int main() {}\n");

    live::LiveSource src({RGV_WATCH_BIN, "--root", r.root}, 5000.0);
    GraphStore       store;
    store.reset(src.baseline());

    const Node* helper = store.node("file:shared/helper.cpp");
    CHECK(helper != nullptr);
    const std::string parent = store.ancestor_of_kind(helper->id, NodeKind::BuildTarget);
    CHECK(parent == "tgt:probe" || parent == "tgt:tool");

    // The other claim is an `owns` edge, never a second parent.
    const Edge* owns = nullptr;
    for (const auto& [id, e] : store.edges()) {
        if (e.kind == EdgeKind::Owns && e.to == "file:shared/helper.cpp") owns = &e;
    }
    CHECK(owns != nullptr);
    CHECK(owns->from != parent);
    CHECK_EQ(owns->evidence->artifact, std::string("CMakeLists.txt"));
}
