#include "rgv/model/GraphStore.h"

#include <algorithm>
#include <sstream>

namespace rgv {
namespace {
const std::vector<EdgeId> kNoEdges{};
const std::vector<NodeId> kNoNodes{};
} // namespace

GraphStore::GraphStore() = default;

void GraphStore::reset(const Snapshot& snap) {
    baseline_   = snap;
    generation_ = snap.generation;

    nodes_.clear();
    edges_.clear();
    changed_.clear();
    adapters_.clear();
    checkpoints_.clear();
    impacts_.clear();
    log_.clear();
    warnings_.clear();

    for (const auto& n : snap.nodes) nodes_[n.id] = n;
    for (const auto& e : snap.edges) edges_[e.id] = e;
    rebuild_indices();

    dirty_.clear();
    dirty_.topology = dirty_.impact = dirty_.changes = dirty_.adapters = true;
    for (const auto& [id, n] : nodes_) dirty_.nodes.insert(id);
    for (const auto& [id, e] : edges_) dirty_.edges.insert(id);
}

void GraphStore::rebuild_indices() {
    out_.clear();
    in_.clear();
    children_.clear();

    for (const auto& [id, e] : edges_) {
        if (!e.active()) continue;
        out_[e.from].push_back(id);
        in_[e.to].push_back(id);
    }
    for (const auto& [id, n] : nodes_) {
        if (!n.parent.empty()) children_[n.parent].push_back(id);
    }
    // Stable order keeps rendering and path enumeration deterministic across runs.
    for (auto& [k, v] : out_) std::sort(v.begin(), v.end());
    for (auto& [k, v] : in_) std::sort(v.begin(), v.end());
    for (auto& [k, v] : children_) std::sort(v.begin(), v.end());
}

const Node* GraphStore::node(const NodeId& id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

const Edge* GraphStore::edge(const EdgeId& id) const {
    auto it = edges_.find(id);
    return it == edges_.end() ? nullptr : &it->second;
}

const std::vector<EdgeId>& GraphStore::out_edges(const NodeId& id) const {
    auto it = out_.find(id);
    return it == out_.end() ? kNoEdges : it->second;
}

const std::vector<EdgeId>& GraphStore::in_edges(const NodeId& id) const {
    auto it = in_.find(id);
    return it == in_.end() ? kNoEdges : it->second;
}

const std::vector<NodeId>& GraphStore::children(const NodeId& id) const {
    auto it = children_.find(id);
    return it == children_.end() ? kNoNodes : it->second;
}

NodeId GraphStore::ancestor_of_kind(const NodeId& id, NodeKind kind) const {
    // Bounded walk: a malformed parent cycle in a fixture must not hang the frame.
    NodeId cur = id;
    for (int guard = 0; guard < 64 && !cur.empty(); ++guard) {
        const Node* n = node(cur);
        if (!n) break;
        if (n->kind == kind) return n->id;
        cur = n->parent;
    }
    return {};
}

const ImpactResult* GraphStore::impact(Level level) const {
    auto it = impacts_.find(level);
    return it == impacts_.end() ? nullptr : &it->second;
}

void GraphStore::insert_node(const Node& n) {
    auto it = nodes_.find(n.id);
    if (it != nodes_.end()) {
        // Reparenting is rare but real (a file moving between packages), so the old
        // child link must be dropped rather than leaving a phantom.
        if (it->second.parent != n.parent && !it->second.parent.empty()) {
            auto& sibs = children_[it->second.parent];
            sibs.erase(std::remove(sibs.begin(), sibs.end(), n.id), sibs.end());
            if (!n.parent.empty()) {
                auto& dst = children_[n.parent];
                dst.insert(std::upper_bound(dst.begin(), dst.end(), n.id), n.id);
            }
        }
        it->second = n;
    } else {
        nodes_[n.id] = n;
        if (!n.parent.empty()) {
            auto& dst = children_[n.parent];
            dst.insert(std::upper_bound(dst.begin(), dst.end(), n.id), n.id);
        }
        dirty_.topology = true;
    }
    dirty_.nodes.insert(n.id);
}

void GraphStore::erase_node(const NodeId& id) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return;

    if (!it->second.parent.empty()) {
        auto& sibs = children_[it->second.parent];
        sibs.erase(std::remove(sibs.begin(), sibs.end(), id), sibs.end());
    }
    // Incident edges cannot outlive their endpoints, or traversal would walk into
    // nodes that no longer exist.
    std::vector<EdgeId> incident;
    for (const auto& eid : out_edges(id)) incident.push_back(eid);
    for (const auto& eid : in_edges(id)) incident.push_back(eid);
    for (const auto& eid : incident) erase_edge(eid);

    children_.erase(id);
    nodes_.erase(it);
    dirty_.nodes.insert(id);
    dirty_.removed_nodes.insert(id);
    dirty_.topology = true;
}

void GraphStore::insert_edge(const Edge& e) {
    auto it = edges_.find(e.id);
    if (it != edges_.end()) {
        const bool was_active = it->second.active();
        if (was_active) detach_edge_index(it->second);
        it->second = e;
    } else {
        edges_[e.id] = e;
        dirty_.topology = true;
    }
    if (e.active()) {
        auto& o = out_[e.from];
        o.insert(std::upper_bound(o.begin(), o.end(), e.id), e.id);
        auto& i = in_[e.to];
        i.insert(std::upper_bound(i.begin(), i.end(), e.id), e.id);
    }
    if (!node(e.from) || !node(e.to)) {
        warnings_.push_back("edge " + e.id + " references a missing endpoint");
    }
    dirty_.edges.insert(e.id);
}

void GraphStore::detach_edge_index(const Edge& e) {
    auto drop = [&](std::unordered_map<NodeId, std::vector<EdgeId>>& idx, const NodeId& key) {
        auto it = idx.find(key);
        if (it == idx.end()) return;
        auto& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), e.id), v.end());
        if (v.empty()) idx.erase(it);
    };
    drop(out_, e.from);
    drop(in_, e.to);
}

void GraphStore::erase_edge(const EdgeId& id) {
    auto it = edges_.find(id);
    if (it == edges_.end()) return;
    detach_edge_index(it->second);
    edges_.erase(it);
    dirty_.edges.insert(id);
    dirty_.removed_edges.insert(id);
    dirty_.topology = true;
}

void GraphStore::apply(const GraphUpdatedPayload& p) {
    for (const auto& n : p.added_nodes) insert_node(n);
    for (const auto& n : p.updated_nodes) insert_node(n);
    for (const auto& e : p.added_edges) insert_edge(e);
    for (const auto& e : p.updated_edges) insert_edge(e);
    // Removals last: an update that reparents a node must land before the old
    // container is torn down.
    for (const auto& id : p.removed_edges) erase_edge(id);
    for (const auto& id : p.removed_nodes) erase_node(id);
}

void GraphStore::apply(const FileChangedPayload& p, Generation gen, double t_ms) {
    auto it = std::find_if(changed_.begin(), changed_.end(),
                           [&](const ChangedFile& c) { return c.node_id == p.node_id; });
    if (it == changed_.end()) {
        changed_.push_back(ChangedFile{p.path, p.node_id, p.change, p.from_path,
                                       p.processing, gen, t_ms});
    } else {
        // A file can be reported many times as it moves through the tiers. Keep the
        // original change kind -- a created-then-modified file is still created.
        it->processing = p.processing;
        it->generation = gen;
        it->t_ms       = t_ms;
        it->path       = p.path;
        if (p.change == FileChangeKind::Deleted) it->change = p.change;
    }
    dirty_.changes = true;
}

void GraphStore::apply(const AdapterStatusPayload& p) {
    auto it = std::find_if(adapters_.begin(), adapters_.end(),
                           [&](const AdapterInfo& a) { return a.name == p.adapter; });
    if (it == adapters_.end()) {
        adapters_.push_back(AdapterInfo{p.adapter, p.state, p.queue_depth,
                                        p.last_success_generation, p.message});
    } else {
        it->state                   = p.state;
        it->queue_depth             = p.queue_depth;
        it->last_success_generation = p.last_success_generation;
        it->message                 = p.message;
    }
    dirty_.adapters = true;
}

void GraphStore::note(const Event& e, std::string summary) {
    log_.push_back(LogEntry{e.t_ms, e.generation, e.type, std::move(summary)});
    if (log_.size() > kMaxLog) log_.pop_front();
}

void GraphStore::on_event(const Event& e) {
    generation_ = std::max(generation_, e.generation);

    switch (e.type) {
        case EventType::SessionStarted: {
            baseline_.session = e.as<SessionStartedPayload>().session;
            note(e, "session " + baseline_.session.name);
            break;
        }
        case EventType::FileChanged: {
            const auto& p = e.as<FileChangedPayload>();
            apply(p, e.generation, e.t_ms);
            note(e, std::string(to_string(p.change)) + " " + p.path + " (" +
                        std::string(to_string(p.processing)) + ")");
            break;
        }
        case EventType::GraphUpdated: {
            const auto& p = e.as<GraphUpdatedPayload>();
            apply(p);
            std::ostringstream os;
            os << "+" << (p.added_nodes.size() + p.added_edges.size())
               << " ~" << (p.updated_nodes.size() + p.updated_edges.size())
               << " -" << (p.removed_nodes.size() + p.removed_edges.size());
            if (!p.note.empty()) os << "  " << p.note;
            note(e, os.str());
            break;
        }
        case EventType::ImpactUpdated: {
            const auto& r = e.as<ImpactResult>();
            impacts_[r.level] = r;   // full replacement for this level
            dirty_.impact     = true;
            note(e, std::string(to_string(r.level)) + " impact: " +
                        std::to_string(r.impacted_nodes.size()) + " node(s)");
            break;
        }
        case EventType::AdapterStatus: {
            const auto& p = e.as<AdapterStatusPayload>();
            apply(p);
            note(e, p.adapter + ": " + std::string(to_string(p.state)) +
                        (p.message.empty() ? "" : " - " + p.message));
            break;
        }
        case EventType::ReconcileCheckpoint: {
            const auto& p = e.as<ReconcileCheckpointPayload>();
            checkpoints_.push_back(e.generation);
            note(e, "reconcile: " + p.message);
            break;
        }
    }
}

} // namespace rgv
