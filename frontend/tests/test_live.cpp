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
    CHECK_EQ(pkg->seed_nodes[0], std::string("pypkg:demo"));
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

    // Packages, nested: api and core sit inside demo, demo inside the src directory.
    const Node* demo = store.node("pypkg:src/demo");
    const Node* api  = store.node("pypkg:src/demo/api");
    const Node* core = store.node("pypkg:src/demo/core");
    CHECK(demo != nullptr && api != nullptr && core != nullptr);
    CHECK(api->kind == NodeKind::Package);
    CHECK_EQ(api->name, std::string("demo.api"));
    CHECK_EQ(api->parent, std::string("pypkg:src/demo"));
    CHECK_EQ(demo->parent, std::string("dir:src"));
    CHECK(store.node("dir:src/demo/api") == nullptr);   // the package replaces the directory

    // Files are owned by the innermost package, which is what FR-11 projects through.
    CHECK_EQ(store.node("file:src/demo/api/routes.py")->parent, std::string("pypkg:src/demo/api"));
    CHECK_EQ(store.ancestor_of_kind("file:src/demo/api/routes.py", NodeKind::Package),
             std::string("pypkg:src/demo/api"));
    CHECK_EQ(store.ancestor_of_kind("file:tests/test_db.py", NodeKind::Package), std::string("pkg:demo"));

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
    CHECK_EQ(api_core->evidence->line, 1);
    CHECK(dep("pypkg:src/demo", "pypkg:src/demo/api") != nullptr);
    CHECK(dep("pkg:demo", "pypkg:src/demo/core") != nullptr);   // the tests, owned by the distribution
    CHECK(dep("pypkg:src/demo/core", "pypkg:src/demo/api") == nullptr);

    // Change db.py: api is directly impacted, demo transitively, and the answer agrees
    // with the frontend's own traversal.
    { std::ofstream(r.root + "/src/demo/core/db.py", std::ios::app) << "# more\n"; }
    CHECK(wait_for(src, store, [&] { return store.impact(Level::Package) != nullptr; }));
    const ImpactResult* pkg = store.impact(Level::Package);
    CHECK_EQ(pkg->seed_nodes.size(), 1u);
    CHECK_EQ(pkg->seed_nodes[0], std::string("pypkg:src/demo/core"));
    std::map<std::string, int> dist;
    for (const auto& n : pkg->impacted_nodes) dist[n.node_id] = n.min_distance;
    CHECK_EQ(dist["pypkg:src/demo/api"], 1);
    CHECK_EQ(dist["pypkg:src/demo"], 2);
    CHECK_EQ(dist["pkg:demo"], 1);

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
