#include "rgv/fixture/FixtureSource.h"

#include "rgv/fixture/Json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <map>

namespace rgv::fixture {
namespace {

namespace fs = std::filesystem;

std::string default_title(const fs::path& p) {
    std::string s = p.stem().string();
    std::replace(s.begin(), s.end(), '-', ' ');
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
}

} // namespace

FixtureSet FixtureSet::load(const std::string& dir) {
    const fs::path root(dir);
    if (!fs::is_directory(root)) throw ParseError(dir + ": not a directory");

    FixtureSet set;
    set.dir      = fs::absolute(root).string();
    set.name     = root.filename().string();
    set.snapshot = load_snapshot((root / "snapshot.json").string());

    // Optional sidecar: titles and ordering for scenarios. Absent is fine -- the
    // filenames then supply the titles.
    std::map<std::string, std::pair<std::string, std::string>> meta;  // file -> {name, desc}
    std::vector<std::string>                                   order;
    const fs::path meta_path = root / "fixture.json";
    if (fs::exists(meta_path)) {
        auto j = nlohmann::json::parse(read_file(meta_path.string()), nullptr, true, true);
        if (auto it = j.find("name"); it != j.end()) set.name = it->get<std::string>();
        if (auto it = j.find("scenarios"); it != j.end()) {
            for (const auto& s : *it) {
                const auto file = s.at("file").get<std::string>();
                meta[file]      = {s.value("name", ""), s.value("description", "")};
                order.push_back(file);
            }
        }
    }

    const fs::path scen_dir = root / "scenarios";
    std::vector<std::string> files;
    if (fs::is_directory(scen_dir)) {
        for (const auto& entry : fs::directory_iterator(scen_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jsonl") {
                files.push_back(entry.path().filename().string());
            }
        }
    }
    std::sort(files.begin(), files.end());

    // Sidecar order wins; anything it does not mention follows alphabetically, so a
    // newly dropped-in scenario file shows up without editing the sidecar.
    std::vector<std::string> ordered;
    for (const auto& f : order) {
        if (std::find(files.begin(), files.end(), f) != files.end()) ordered.push_back(f);
    }
    for (const auto& f : files) {
        if (std::find(ordered.begin(), ordered.end(), f) == ordered.end()) ordered.push_back(f);
    }

    for (const auto& f : ordered) {
        const fs::path p = scen_dir / f;
        Scenario       s;
        s.path   = p.string();
        s.events = load_scenario(s.path);
        if (auto it = meta.find(f); it != meta.end()) {
            s.name        = it->second.first.empty() ? default_title(p) : it->second.first;
            s.description = it->second.second;
        } else {
            s.name = default_title(p);
        }
        set.scenarios.push_back(std::move(s));
    }

    if (set.scenarios.empty()) {
        // An empty timeline is still a valid thing to look at: the baseline graph.
        set.scenarios.push_back(Scenario{"baseline only", "No scenario files found.", "", {}});
    }
    return set;
}

FixtureSource::FixtureSource(FixtureSet set, std::size_t scenario_index)
    : set_(std::move(set)) {
    index_ = std::min(scenario_index, set_.scenarios.size() - 1);
    rebuild_markers();
    restart();
}

void FixtureSource::rebuild_markers() {
    markers_.clear();
    for (const auto& e : scenario().events) {
        switch (e.type) {
            case EventType::FileChanged: {
                const auto& p = e.as<FileChangedPayload>();
                if (p.processing != Processing::Pending) break;  // only mark the first beat
                markers_.push_back({e.t_ms, std::string(to_string(p.change)) + " " + p.path});
                break;
            }
            case EventType::GraphUpdated: {
                const auto& p = e.as<GraphUpdatedPayload>();
                if (!p.note.empty()) markers_.push_back({e.t_ms, p.note});
                break;
            }
            case EventType::ReconcileCheckpoint:
                markers_.push_back({e.t_ms, "reconcile"});
                break;
            default:
                break;
        }
    }
}

void FixtureSource::select_scenario(std::size_t i) {
    if (i >= set_.scenarios.size() || i == index_) return;
    index_ = i;
    rebuild_markers();
    restart();
}

bool FixtureSource::take_reset() {
    const bool r = needs_reset_;
    needs_reset_ = false;
    return r;
}

void FixtureSource::set_rate(double r) { rate_ = std::clamp(r, 0.25, 8.0); }

void FixtureSource::restart() {
    cursor_ms_   = 0.0;
    next_        = 0;
    flush_until_ = 0;
    playing_     = true;
    needs_reset_ = true;
}

void FixtureSource::seek_ms(double ms) {
    const auto& ev = scenario().events;
    cursor_ms_     = std::max(0.0, ms);

    // Always rewind and re-apply from the baseline. Scenarios are small, and this
    // makes a seek land in exactly the state a forward play would have produced --
    // no incremental undo logic to get subtly wrong.
    next_        = 0;
    needs_reset_ = true;

    std::size_t target = 0;
    while (target < ev.size() && ev[target].t_ms <= cursor_ms_) ++target;
    flush_until_ = target;
}

void FixtureSource::step_event() {
    const auto& ev = scenario().events;
    if (next_ >= ev.size()) return;
    playing_     = false;
    cursor_ms_   = ev[next_].t_ms;
    flush_until_ = next_ + 1;
}

int FixtureSource::poll(double dt_seconds, EventSink& sink) {
    const auto& ev = scenario().events;
    int         emitted = 0;

    // Events the timeline jumped over are delivered immediately: a seek must not
    // wait for wall-clock time to catch up.
    while (next_ < flush_until_ && next_ < ev.size()) {
        sink.on_event(ev[next_]);
        ++next_;
        ++emitted;
    }
    flush_until_ = next_;

    if (playing_ && next_ < ev.size()) {
        cursor_ms_ += dt_seconds * 1000.0 * rate_;
        while (next_ < ev.size() && ev[next_].t_ms <= cursor_ms_) {
            sink.on_event(ev[next_]);
            ++next_;
            ++emitted;
        }
    } else if (playing_) {
        // Past the last event: let the cursor run on briefly so the scrubber does not
        // snap to the end the instant the final event fires.
        cursor_ms_ = std::min(cursor_ms_ + dt_seconds * 1000.0 * rate_,
                              scenario().duration_ms() + 1000.0);
    }
    return emitted;
}

SourceStatus FixtureSource::status() const {
    const auto&  ev = scenario().events;
    SourceStatus s;
    s.attached       = true;
    s.ended          = next_ >= ev.size();
    s.generation     = next_ > 0 ? ev[next_ - 1].generation : set_.snapshot.generation;
    s.position_ms    = cursor_ms_;
    s.duration_ms    = scenario().duration_ms();
    s.events_emitted = static_cast<int>(next_);
    s.events_total   = static_cast<int>(ev.size());
    s.description    = set_.name + " / " + scenario().name;
    return s;
}

} // namespace rgv::fixture
