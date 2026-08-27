// Timeline behaviour, checked against the real committed fixtures. If a scenario file
// is edited into an inconsistent state these fail, which is the point.
#include "TestMain.h"

#include "rgv/fixture/FixtureSource.h"
#include "rgv/model/GraphStore.h"

#include <map>
#include <sstream>

#ifndef RGV_FIXTURE_DIR
#define RGV_FIXTURE_DIR "fixtures"
#endif

using namespace rgv;

namespace {

const std::string kTsFixture = std::string(RGV_FIXTURE_DIR) + "/monorepo-ts";

// A canonical string for the whole visible state. Two timelines that reach the same
// point must produce identical text, which is a much stronger claim than comparing
// node counts.
std::string fingerprint(const GraphStore& s) {
    std::map<std::string, std::string> lines;
    for (const auto& [id, n] : s.nodes()) {
        lines["n:" + id] = std::string(to_string(n.kind)) + "|" + n.parent + "|" +
                           std::string(to_string(n.freshness));
    }
    for (const auto& [id, e] : s.edges()) {
        lines["e:" + id] = e.from + "->" + e.to + "|" + std::string(to_string(e.kind)) +
                           "|" + std::string(to_string(e.confidence)) + "|" +
                           std::string(to_string(e.freshness));
    }
    std::ostringstream os;
    os << "gen=" << s.generation() << "\n";
    for (const auto& [k, v] : lines) os << k << " " << v << "\n";
    for (const auto& c : s.changed_files()) {
        os << "c:" << c.path << " " << to_string(c.change) << " " << to_string(c.processing) << "\n";
    }
    for (const auto& [lvl, idx] : std::map<Level, int>{{Level::Package, 0}, {Level::File, 1}}) {
        if (const ImpactResult* r = s.impact(lvl)) {
            for (const auto& n : r->impacted_nodes) {
                os << "i:" << to_string(lvl) << ":" << n.node_id << " " << n.min_distance
                   << " " << to_string(n.cause) << " " << to_string(n.freshness) << "\n";
            }
        }
    }
    return os.str();
}

void pump(fixture::FixtureSource& src, GraphStore& store, double dt, int steps) {
    for (int i = 0; i < steps; ++i) {
        if (src.take_reset()) store.reset(src.baseline());
        src.poll(dt, store);
    }
}

std::string play_to_end(fixture::FixtureSet set, std::size_t idx, double dt) {
    fixture::FixtureSource src(set, idx);
    GraphStore             store;
    for (int i = 0; i < 20000; ++i) {
        if (src.take_reset()) store.reset(src.baseline());
        src.poll(dt, store);
        if (src.status().ended) break;
    }
    return fingerprint(store);
}

} // namespace

TEST(fixture_set_loads_every_committed_scenario) {
    auto set = fixture::FixtureSet::load(kTsFixture);
    CHECK(set.scenarios.size() >= 6);
    CHECK(!set.snapshot.nodes.empty());
    // The sidecar supplies titles; a scenario with a bare filename title means
    // fixture.json and the scenarios directory have drifted apart.
    for (const auto& s : set.scenarios) CHECK(!s.name.empty());
}

// The frame delta must not change the outcome. If it does, some state depends on how
// many events happened to land in one poll -- a bug that only shows up under load.
TEST(replay_is_independent_of_frame_rate) {
    auto set = fixture::FixtureSet::load(kTsFixture);
    for (std::size_t i = 0; i < set.scenarios.size(); ++i) {
        const std::string slow = play_to_end(set, i, 1.0 / 240.0);
        const std::string fast = play_to_end(set, i, 1.0 / 15.0);
        const std::string huge = play_to_end(set, i, 2.5);
        CHECK(slow == fast);
        CHECK(slow == huge);
    }
}

// Seeking must land in exactly the state a forward play would have produced. This is
// what makes the scrubber trustworthy for reviewing a scenario.
TEST(seeking_lands_in_the_same_state_as_playing_forward) {
    auto set = fixture::FixtureSet::load(kTsFixture);
    for (std::size_t i = 0; i < set.scenarios.size(); ++i) {
        fixture::FixtureSource src(set, i);
        const double target = src.scenario().duration_ms() * 0.6;

        // played forward, paused at the target
        GraphStore forward;
        src.timeline()->set_rate(1.0);
        for (int k = 0; k < 20000; ++k) {
            if (src.take_reset()) forward.reset(src.baseline());
            src.poll(1.0 / 120.0, forward);
            if (src.status().position_ms >= target) break;
        }
        // Only compare once the cursor is past the same events, not the same instant.
        const int emitted_forward = src.status().events_emitted;

        fixture::FixtureSource src2(set, i);
        GraphStore             seeked;
        src2.timeline()->pause();
        src2.timeline()->seek_ms(target);
        pump(src2, seeked, 0.0, 2);

        // Forward play overshoots by at most the events inside one frame; align by
        // stepping the seeked source to the same event count.
        while (src2.status().events_emitted < emitted_forward) {
            src2.timeline()->step_event();
            pump(src2, seeked, 0.0, 1);
        }
        CHECK(fingerprint(forward) == fingerprint(seeked));
    }
}

TEST(restart_returns_to_the_baseline) {
    auto                   set = fixture::FixtureSet::load(kTsFixture);
    fixture::FixtureSource src(set, 0);
    GraphStore             store;

    GraphStore baseline_only;
    baseline_only.reset(set.snapshot);
    const std::string at_baseline = fingerprint(baseline_only);

    for (int i = 0; i < 20000; ++i) {
        if (src.take_reset()) store.reset(src.baseline());
        src.poll(0.05, store);
        if (src.status().ended) break;
    }
    CHECK(fingerprint(store) != at_baseline);

    src.timeline()->restart();
    src.timeline()->pause();
    pump(src, store, 0.0, 2);
    CHECK(fingerprint(store) == at_baseline);
}

TEST(step_event_advances_exactly_one_event) {
    auto                   set = fixture::FixtureSet::load(kTsFixture);
    fixture::FixtureSource src(set, 0);
    GraphStore             store;
    src.timeline()->pause();
    pump(src, store, 0.0, 1);
    CHECK_EQ(src.status().events_emitted, 0);

    for (int i = 1; i <= 5; ++i) {
        src.timeline()->step_event();
        pump(src, store, 0.0, 1);
        CHECK_EQ(src.status().events_emitted, i);
    }
}

// Switching scenarios must not leave state from the previous one on screen.
TEST(switching_scenario_rewinds_to_the_baseline) {
    auto                   set = fixture::FixtureSet::load(kTsFixture);
    fixture::FixtureSource src(set, 0);
    GraphStore             store;
    for (int i = 0; i < 20000; ++i) {
        if (src.take_reset()) store.reset(src.baseline());
        src.poll(0.05, store);
        if (src.status().ended) break;
    }
    CHECK(!store.changed_files().empty());

    src.select_scenario(1);
    src.timeline()->pause();
    pump(src, store, 0.0, 2);
    CHECK(store.changed_files().empty());
    CHECK_EQ(store.generation(), set.snapshot.generation);
}

TEST(markers_are_ordered_and_cover_the_scenario_beats) {
    auto                   set = fixture::FixtureSet::load(kTsFixture);
    fixture::FixtureSource src(set, 0);
    const auto&            m = src.timeline()->markers();
    CHECK(!m.empty());
    for (std::size_t i = 1; i < m.size(); ++i) CHECK(m[i - 1].t_ms <= m[i].t_ms);
    for (const auto& mk : m) CHECK(!mk.label.empty());
}

TEST(rate_is_clamped_to_a_usable_range) {
    auto                   set = fixture::FixtureSet::load(kTsFixture);
    fixture::FixtureSource src(set, 0);
    src.timeline()->set_rate(1000.0);
    CHECK(src.timeline()->rate() <= 8.0);
    src.timeline()->set_rate(0.0001);
    CHECK(src.timeline()->rate() >= 0.25);
}

// Scenario 3 is the stale-evidence case. It is the one a renderer is most likely to
// get wrong, so the fixture itself is asserted rather than just replayed.
TEST(invalid_intermediate_scenario_produces_stale_not_missing_edges) {
    auto set = fixture::FixtureSet::load(kTsFixture);
    std::size_t idx = 0;
    for (std::size_t i = 0; i < set.scenarios.size(); ++i) {
        if (set.scenarios[i].path.find("03-invalid") != std::string::npos) idx = i;
    }
    fixture::FixtureSource src(set, idx);
    GraphStore             store;
    src.timeline()->pause();

    const std::size_t before = set.snapshot.edges.size();
    int               max_stale = 0;
    for (int k = 0; k < 400 && !src.status().ended; ++k) {
        src.timeline()->step_event();
        if (src.take_reset()) store.reset(src.baseline());
        src.poll(0.0, store);

        int stale = 0;
        for (const auto& [id, e] : store.edges()) {
            if (e.freshness == Freshness::Stale) ++stale;
        }
        max_stale = std::max(max_stale, stale);
        // The edge count must never dip: a failed parse may downgrade evidence but
        // must never delete a relationship.
        CHECK(store.edges().size() >= before);
    }
    CHECK(max_stale > 0);
    // and it recovers
    for (const auto& [id, e] : store.edges()) CHECK(e.freshness != Freshness::Stale);
}
