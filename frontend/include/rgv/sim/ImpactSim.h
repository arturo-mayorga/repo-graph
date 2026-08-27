// Frontend-side simulation of what the backend's blast-radius engine would return.
//
// This is NOT part of the contract. It exists for two reasons:
//   1. UX exploration -- click any node and see its reverse closure without having to
//      author an impact.updated event for that case.
//   2. Fixture authoring -- generate the impact block for a scenario instead of
//      hand-computing paths.
// When a real backend is attached, impact arrives via impact.updated and this is used
// only for speculative "what if I changed this?" probing.
#pragma once

#include "rgv/contract/Impact.h"
#include "rgv/model/GraphStore.h"

namespace rgv::sim {

// Reverse-reachable closure of `seeds` over the traversable edge kinds in `filters`,
// with a shortest path recorded per impacted node (spec section 10.1).
//
// `level` selects which node kind participates: seeds are projected up to that level
// via the containment tree, and traversal only visits nodes of that kind. That
// projection is what makes "a changed file impacts these packages" a single query.
ImpactResult compute(const GraphStore& store,
                     const std::vector<NodeId>& seeds,
                     Level level,
                     const ImpactFilters& filters);

// Seeds implied by the session's changed-file list, projected to `level`.
std::vector<NodeId> seeds_for_level(const GraphStore& store, Level level);

} // namespace rgv::sim
