// Blast radius, provider-side.
//
// The contract makes impact the provider's to compute (§4.3, §6.4): the frontend
// renders `impact.updated` and does not derive one from a live stream. This is the
// reverse reachability the spec describes (§10.1) over whichever edges the caller
// hands in -- `imports` for the file level, `depends_on` for packages -- with one
// shortest path per impacted node so the frontend can explain every hit (FR-26).
#pragma once

#include <string>
#include <vector>

namespace rgv::watch {

struct GraphEdge {
    std::string id;
    std::string from;   // dependent
    std::string to;     // dependency
    long        valid_from = 0;
    bool        heuristic  = false;   // excluded from traversal (contract §2.4)
};

struct ImpactHit {
    std::string              node;
    int                      distance = 0;   // 0 for a seed
    std::vector<std::string> path;           // edge ids from this node toward a seed
    // FR-29: the hit exists only because an edge appeared after the baseline. A node
    // whose path is entirely baseline edges is impacted by a changed implementation.
    bool dependency_added = false;
};

// Every node from which a seed is reachable by following edges dependent -> dependency,
// seeds included at distance 0. Sorted by node id so a stream is deterministic.
std::vector<ImpactHit> reverse_reach(const std::vector<GraphEdge>&   edges,
                                     const std::vector<std::string>& seeds,
                                     long                            baseline_generation,
                                     int                             max_depth);

} // namespace rgv::watch
