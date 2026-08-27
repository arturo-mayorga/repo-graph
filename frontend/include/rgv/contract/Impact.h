// Blast-radius result (spec section 10.3).
//
// A path is an ordered list of EDGE ids running from the impacted node toward a seed.
// Edge ids rather than node ids, because the UI must be able to open provenance for
// every hop in the explanation without a second round-trip.
#pragma once

#include "rgv/contract/Types.h"

#include <string>
#include <vector>

namespace rgv {

struct ImpactPath {
    std::vector<EdgeId> edges;
};

struct ImpactedNode {
    NodeId      node_id;
    int         min_distance = 0;
    bool        direct       = false;
    bool        changed      = false;   // this node is itself a seed
    Freshness   freshness    = Freshness::Current;
    ImpactCause cause        = ImpactCause::Implementation;

    std::vector<ImpactPath> paths;      // at least one, per FR-26
    bool paths_truncated = false;       // backend capped the enumeration
};

struct ImpactFilters {
    int                   max_depth         = 8;
    std::vector<EdgeKind> edge_kinds{EdgeKind::DependsOn};
    bool                  include_heuristic = false;
};

struct ImpactResult {
    Level                     level = Level::Package;
    Generation                graph_generation    = 0;
    Generation                baseline_generation = 0;
    std::vector<NodeId>       seed_nodes;
    ImpactFilters             filters;
    std::vector<ImpactedNode> impacted_nodes;
};

} // namespace rgv
