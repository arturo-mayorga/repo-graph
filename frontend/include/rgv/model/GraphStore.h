// GraphStore is the single writer of graph state. It consumes the event stream and
// exposes dirty sets so ECS sync and layout touch only what actually moved -- the
// mechanism behind "a file save must not trigger a full global layout" (spec 11.2).
#pragma once

#include "rgv/contract/Event.h"
#include "rgv/contract/Graph.h"
#include "rgv/contract/IGraphSource.h"
#include "rgv/contract/Impact.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rgv {

// A file the agent touched during this session, plus where it is in the pipeline.
struct ChangedFile {
    std::string    path;
    NodeId         node_id;
    FileChangeKind change     = FileChangeKind::Modified;
    std::string    from_path;
    Processing     processing = Processing::Pending;
    Generation     generation = 0;
    double         t_ms       = 0.0;
};

struct AdapterInfo {
    std::string  name;
    AdapterState state                   = AdapterState::Idle;
    int          queue_depth             = 0;
    Generation   last_success_generation = 0;
    std::string  message;
};

// One line in the UI's event log. Kept separate from Event so the log survives
// without pinning whole payloads in memory.
struct LogEntry {
    double      t_ms       = 0.0;
    Generation  generation = 0;
    EventType   type       = EventType::FileChanged;
    std::string summary;
};

// What changed since the last clear_dirty(). Consumers diff against this instead of
// walking the whole graph.
struct DirtySet {
    std::unordered_set<NodeId> nodes;          // added / updated / removed
    std::unordered_set<EdgeId> edges;
    std::unordered_set<NodeId> removed_nodes;  // subset of `nodes`, already gone
    std::unordered_set<EdgeId> removed_edges;
    bool topology = false;   // node/edge set changed => layout may need re-solving
    bool impact   = false;   // an impact result was replaced
    bool changes  = false;   // the changed-file list moved
    bool adapters = false;

    bool any() const {
        return topology || impact || changes || adapters || !nodes.empty() || !edges.empty();
    }
    void clear() {
        nodes.clear(); edges.clear(); removed_nodes.clear(); removed_edges.clear();
        topology = impact = changes = adapters = false;
    }
};

class GraphStore final : public EventSink {
public:
    GraphStore();

    // Install a baseline. Clears all live state and marks everything dirty.
    void reset(const Snapshot& snap);

    void on_event(const Event& e) override;

    // -- graph access --------------------------------------------------------
    const Node* node(const NodeId& id) const;
    const Edge* edge(const EdgeId& id) const;

    const std::unordered_map<NodeId, Node>& nodes() const { return nodes_; }
    const std::unordered_map<EdgeId, Edge>& edges() const { return edges_; }

    // Dependent -> dependency. Traversing these forward answers "what does X need".
    const std::vector<EdgeId>& out_edges(const NodeId& id) const;
    // Dependency -> dependent. Traversing these is the blast radius direction.
    const std::vector<EdgeId>& in_edges(const NodeId& id) const;
    // Containment tree, for collapse/expand and projection to owning package.
    const std::vector<NodeId>& children(const NodeId& id) const;

    // Walk `parent` upward until a node of `kind` is found ("" if none). This is the
    // file -> owning package projection (FR-11).
    NodeId ancestor_of_kind(const NodeId& id, NodeKind kind) const;

    // -- session state -------------------------------------------------------
    const Snapshot&                 baseline() const { return baseline_; }
    Generation                      generation() const { return generation_; }
    const std::vector<ChangedFile>& changed_files() const { return changed_; }
    const std::vector<AdapterInfo>& adapters() const { return adapters_; }
    const std::deque<LogEntry>&     log() const { return log_; }

    // Latest impact result per level. Results for different levels coexist so
    // switching abstraction (FR-30) is instant and lossless.
    const ImpactResult* impact(Level level) const;

    // Generations marked Git-authoritative by a reconcile checkpoint (spec T3).
    const std::vector<Generation>& checkpoints() const { return checkpoints_; }

    // -- dirty tracking ------------------------------------------------------
    const DirtySet& dirty() const { return dirty_; }
    void            clear_dirty() { dirty_.clear(); }

    // Diagnostics for malformed fixtures: edges whose endpoints are missing.
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    void apply(const GraphUpdatedPayload& p);
    void apply(const FileChangedPayload& p, Generation gen, double t_ms);
    void apply(const AdapterStatusPayload& p);
    void insert_node(const Node& n);
    void erase_node(const NodeId& id);
    void insert_edge(const Edge& e);
    void erase_edge(const EdgeId& id);
    void detach_edge_index(const Edge& e);
    void rebuild_indices();
    void note(const Event& e, std::string summary);

    Snapshot   baseline_;
    Generation generation_ = 0;

    std::unordered_map<NodeId, Node> nodes_;
    std::unordered_map<EdgeId, Edge> edges_;

    std::unordered_map<NodeId, std::vector<EdgeId>> out_;
    std::unordered_map<NodeId, std::vector<EdgeId>> in_;
    std::unordered_map<NodeId, std::vector<NodeId>> children_;

    std::vector<ChangedFile>   changed_;
    std::vector<AdapterInfo>   adapters_;
    std::vector<Generation>    checkpoints_;
    std::map<Level, ImpactResult> impacts_;

    std::deque<LogEntry>     log_;
    std::vector<std::string> warnings_;
    DirtySet                 dirty_;

    static constexpr std::size_t kMaxLog = 512;
};

} // namespace rgv
