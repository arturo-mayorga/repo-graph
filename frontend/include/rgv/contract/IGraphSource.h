// The one interface the frontend consumes. FixtureSource and (later) LiveSource both
// implement it, and no frontend code may branch on which is attached.
#pragma once

#include "rgv/contract/Event.h"
#include "rgv/contract/Graph.h"

#include <string>

namespace rgv {

struct SourceStatus {
    bool        attached    = false;
    bool        ended       = false;  // replayable source reached end of scenario
    Generation  generation  = 0;
    double      position_ms = 0.0;
    double      duration_ms = 0.0;    // 0 for live sources
    int         events_emitted = 0;
    int         events_total   = 0;   // 0 for live sources
    std::string description;
};

// Non-null only for replayable sources. Its presence is what tells the UI to draw a
// scrubber -- that is the single sanctioned way to detect a fixture, and it is a
// capability check rather than a type check.
struct Timeline {
    virtual ~Timeline() = default;

    virtual void   play()            = 0;
    virtual void   pause()           = 0;
    virtual bool   playing() const   = 0;
    virtual void   set_rate(double)  = 0;   // clamped to [0.25, 8.0]
    virtual double rate() const      = 0;

    // Seeking rewinds to the baseline and re-applies events up to the target, so the
    // resulting state is identical to having played forward. Cheap because scenarios
    // are small; a live source has no timeline and never pays this.
    virtual void seek_ms(double ms)  = 0;
    virtual void step_event()        = 0;   // advance exactly one event
    virtual void restart()           = 0;

    // Ordered marks for the scrubber, so a reviewer can jump between beats.
    struct Marker {
        double      t_ms = 0.0;
        std::string label;
    };
    virtual const std::vector<Marker>& markers() const = 0;
};

class IGraphSource {
public:
    virtual ~IGraphSource() = default;

    // Baseline graph. Valid from attach until detach; never mutated by events.
    virtual const Snapshot& baseline() const = 0;

    // Drain every event that has become due. Called once per frame with the frame
    // delta. Must be non-blocking. Returns the number of events emitted.
    virtual int poll(double dt_seconds, EventSink& sink) = 0;

    virtual SourceStatus status() const = 0;

    virtual Timeline* timeline() { return nullptr; }
};

} // namespace rgv
