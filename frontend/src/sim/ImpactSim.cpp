#include "rgv/sim/ImpactSim.h"

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace rgv::sim {
namespace {

NodeKind kind_for_level(Level l) {
    switch (l) {
        case Level::Package:     return NodeKind::Package;
        case Level::BuildTarget: return NodeKind::BuildTarget;
        case Level::File:        return NodeKind::File;
        case Level::Symbol:      return NodeKind::Symbol;
    }
    return NodeKind::Package;
}

bool traversable(const Edge& e, const ImpactFilters& f) {
    if (!e.active()) return false;
    if (!is_dependency_edge(e.kind)) return false;   // structural edges are never impact
    if (std::find(f.edge_kinds.begin(), f.edge_kinds.end(), e.kind) == f.edge_kinds.end()) {
        return false;
    }
    if (!f.include_heuristic &&
        (e.confidence == Confidence::Heuristic || e.confidence == Confidence::Unresolved)) {
        return false;
    }
    // An invalid provider result is not evidence of a relationship. Stale is: it was
    // true at last known good, and hiding it would understate the blast radius.
    return e.freshness != Freshness::Invalid;
}

// A node as it takes part at `level`: itself when its kind is admitted there, else
// its nearest ancestor of the level's kind. A changed file is its own seed at the
// symbol level -- files take part there -- and its package at the package level.
NodeId project(const GraphStore& store, const NodeId& id, Level level) {
    if (const Node* n = store.node(id); n && level_admits(level, n->kind)) return id;
    return store.ancestor_of_kind(id, kind_for_level(level));
}

} // namespace

std::vector<NodeId> seeds_for_level(const GraphStore& store, Level level) {
    std::vector<NodeId> seeds;
    std::unordered_set<NodeId> seen;

    for (const auto& c : store.changed_files()) {
        NodeId projected = project(store, c.node_id, level);
        if (projected.empty()) continue;
        if (seen.insert(projected).second) seeds.push_back(projected);
    }
    std::sort(seeds.begin(), seeds.end());
    return seeds;
}

ImpactResult compute(const GraphStore& store,
                     const std::vector<NodeId>& raw_seeds,
                     Level level,
                     const ImpactFilters& filters) {
    ImpactResult r;
    r.level               = level;
    r.filters             = filters;
    r.graph_generation    = store.generation();
    r.baseline_generation = store.baseline().session.baseline_generation;

    // Project every seed up to the requested level, so a changed file can seed a
    // package-level query without the caller doing the lookup.
    std::unordered_set<NodeId> seed_set;
    for (const auto& s : raw_seeds) {
        NodeId p = project(store, s, level);
        if (!p.empty()) seed_set.insert(p);
    }
    r.seed_nodes.assign(seed_set.begin(), seed_set.end());
    std::sort(r.seed_nodes.begin(), r.seed_nodes.end());
    if (r.seed_nodes.empty()) return r;

    struct Visit {
        int    distance = 0;
        EdgeId via;       // edge taken to reach this node (dependent -> dependency)
        NodeId from;      // the node we reached it from, i.e. one step closer to a seed
    };
    std::unordered_map<NodeId, Visit> visited;
    std::deque<NodeId>                queue;

    for (const auto& s : r.seed_nodes) {
        visited[s] = Visit{0, {}, {}};
        queue.push_back(s);
    }

    // Reverse BFS: in_edges(n) are edges whose `to` is n, i.e. things that depend on n.
    // BFS order guarantees the first time a node is reached is its minimum distance.
    while (!queue.empty()) {
        const NodeId cur = queue.front();
        queue.pop_front();
        const int dist = visited[cur].distance;
        if (dist >= filters.max_depth) continue;

        for (const auto& eid : store.in_edges(cur)) {
            const Edge* e = store.edge(eid);
            if (!e || !traversable(*e, filters)) continue;

            const Node* dependent = store.node(e->from);
            if (!dependent || !level_admits(level, dependent->kind)) continue;
            if (visited.count(e->from)) continue;

            visited[e->from] = Visit{dist + 1, eid, cur};
            queue.push_back(e->from);
        }
    }

    for (const auto& [id, v] : visited) {
        ImpactedNode in;
        in.node_id      = id;
        in.min_distance = v.distance;
        in.direct       = v.distance == 1;
        in.changed      = v.distance == 0;
        if (const Node* n = store.node(id)) in.freshness = n->freshness;

        // Walk the BFS parent chain back to a seed, collecting the edges. Stored
        // impacted-node-first so it reads as "api depends on auth, which changed".
        NodeId walk = id;
        ImpactPath path;
        while (visited[walk].distance > 0) {
            path.edges.push_back(visited[walk].via);
            walk = visited[walk].from;
        }
        // A stale edge anywhere on the path makes the whole conclusion stale; saying
        // otherwise would present stale evidence as current (NFR-04).
        for (const auto& eid : path.edges) {
            if (const Edge* e = store.edge(eid); e && e->freshness == Freshness::Stale) {
                in.freshness = Freshness::Stale;
            }
        }
        if (!path.edges.empty()) in.paths.push_back(std::move(path));
        // Only one shortest path is enumerated here; the real engine returns several
        // and sets paths_truncated when it caps.
        in.paths_truncated = false;
        r.impacted_nodes.push_back(std::move(in));
    }

    std::sort(r.impacted_nodes.begin(), r.impacted_nodes.end(),
              [](const ImpactedNode& a, const ImpactedNode& b) {
                  if (a.min_distance != b.min_distance) return a.min_distance < b.min_distance;
                  return a.node_id < b.node_id;
              });
    return r;
}

} // namespace rgv::sim
