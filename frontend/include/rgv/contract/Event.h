// The event stream. Deltas, never full snapshots -- a file save must not reserialize
// the graph (NFR-01).
#pragma once

#include "rgv/contract/Graph.h"
#include "rgv/contract/Impact.h"
#include "rgv/contract/Types.h"

#include <string>
#include <variant>
#include <vector>

namespace rgv {

enum class EventType {
    SessionStarted,
    FileChanged,
    GraphUpdated,
    ImpactUpdated,
    AdapterStatus,
    ReconcileCheckpoint,
};

std::string_view to_string(EventType t);
EventType        parse_event_type(std::string_view s, bool* ok = nullptr);

struct SessionStartedPayload {
    SessionInfo session;
};

// T0 change truth. `processing` is the frontend's only signal that work is still
// outstanding for one specific file.
struct FileChangedPayload {
    std::string    path;
    NodeId         node_id;
    FileChangeKind change     = FileChangeKind::Modified;
    std::string    from_path;             // set when change == Renamed
    Processing     processing = Processing::Pending;
};

// Removal semantics matter (FR-19):
//   - a relationship that disappeared from the source  -> removed_edges
//   - a relationship whose evidence merely went stale   -> updated_edges w/ Stale
// The frontend renders these differently; conflating them produces false deletions.
struct GraphUpdatedPayload {
    std::vector<Node>   added_nodes;
    std::vector<NodeId> removed_nodes;
    std::vector<Node>   updated_nodes;    // full replacement by id
    std::vector<Edge>   added_edges;
    std::vector<EdgeId> removed_edges;
    std::vector<Edge>   updated_edges;    // full replacement by id
    std::string         note;             // surfaced in the event log
};

struct AdapterStatusPayload {
    std::string  adapter;
    AdapterState state                   = AdapterState::Idle;
    int          queue_depth             = 0;
    Generation   last_success_generation = 0;
    std::string  message;
};

struct ReconcileCheckpointPayload {
    int         corrections = 0;
    std::string message;
};

using EventPayload = std::variant<SessionStartedPayload,
                                  FileChangedPayload,
                                  GraphUpdatedPayload,
                                  ImpactResult,
                                  AdapterStatusPayload,
                                  ReconcileCheckpointPayload>;

struct Event {
    // Scheduled offset from scenario start. Fixture-only: LiveSource sets it to
    // wall-clock arrival, and correctness must never depend on it -- only the scrubber.
    double       t_ms       = 0.0;
    EventType    type       = EventType::FileChanged;
    Generation   generation = 0;
    EventPayload payload;

    template <class T> const T& as() const { return std::get<T>(payload); }
    template <class T> bool     is() const { return std::holds_alternative<T>(payload); }
};

struct EventSink {
    virtual ~EventSink() = default;
    virtual void on_event(const Event& e) = 0;
};

} // namespace rgv
