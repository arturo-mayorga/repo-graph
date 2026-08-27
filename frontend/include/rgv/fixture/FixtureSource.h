// Replays an authored snapshot + scenario as if it were a live backend.
//
// This is the only IGraphSource that exists today, and it is what all UX work runs
// against. When LiveSource lands, the app swaps the pointer and nothing else changes.
#pragma once

#include "rgv/contract/IGraphSource.h"

#include <memory>
#include <string>
#include <vector>

namespace rgv::fixture {

// A scenario is a named timeline over one snapshot. Several scenarios can share a
// snapshot so a reviewer can flip between "what if the agent did X" cases.
struct Scenario {
    std::string        name;
    std::string        description;
    std::string        path;
    std::vector<Event> events;

    double duration_ms() const { return events.empty() ? 0.0 : events.back().t_ms; }
};

// Discovers `snapshot.json` + `scenarios/*.jsonl` under a fixture directory, plus an
// optional `fixture.json` supplying scenario titles and descriptions.
struct FixtureSet {
    std::string           name;
    std::string           dir;
    Snapshot              snapshot;
    std::vector<Scenario> scenarios;

    static FixtureSet load(const std::string& dir);
};

class FixtureSource final : public IGraphSource, private Timeline {
public:
    // `set` is copied, so the source owns a stable baseline for the whole session.
    FixtureSource(FixtureSet set, std::size_t scenario_index = 0);

    // -- IGraphSource --------------------------------------------------------
    const Snapshot& baseline() const override { return set_.snapshot; }
    int             poll(double dt_seconds, EventSink& sink) override;
    SourceStatus    status() const override;
    Timeline*       timeline() override { return this; }

    // -- fixture selection ---------------------------------------------------
    const FixtureSet&            set() const { return set_; }
    const std::vector<Scenario>& scenarios() const { return set_.scenarios; }
    std::size_t                  scenario_index() const { return index_; }
    const Scenario&              scenario() const { return set_.scenarios[index_]; }

    // Switching scenario rewinds to the baseline. The caller must re-seed its store
    // from baseline() before the next poll, which `needs_reset()` signals.
    void select_scenario(std::size_t i);

    // True after a reset-inducing operation (scenario switch, restart, backward seek).
    // The app clears it by calling take_reset().
    bool needs_reset() const { return needs_reset_; }
    bool take_reset();

private:
    // -- Timeline ------------------------------------------------------------
    void   play() override { playing_ = true; }
    void   pause() override { playing_ = false; }
    bool   playing() const override { return playing_; }
    void   set_rate(double r) override;
    double rate() const override { return rate_; }
    void   seek_ms(double ms) override;
    void   step_event() override;
    void   restart() override;
    const std::vector<Timeline::Marker>& markers() const override { return markers_; }

    void rebuild_markers();

    FixtureSet  set_;
    std::size_t index_ = 0;

    double      cursor_ms_  = 0.0;
    std::size_t next_       = 0;   // index of the next event to emit
    bool        playing_    = true;
    double      rate_       = 1.0;
    bool        needs_reset_= true;

    // Set by seek_ms/step_event: events the timeline jumped over still have to reach
    // the store, so poll() flushes them on the next frame without waiting for time.
    std::size_t flush_until_ = 0;

    std::vector<Timeline::Marker> markers_;
};

} // namespace rgv::fixture
