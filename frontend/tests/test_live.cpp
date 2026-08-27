// The live transport (contract 6). These run a real child process, because the whole
#include <unistd.h>
// point of this class is what happens across a pipe: partial reads, a provider that
// exits, a line that arrives in two pieces.

#include "TestMain.h"

#include "rgv/live/LiveSource.h"
#include "rgv/model/GraphStore.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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
