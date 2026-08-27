// Reach: how much of the repository ultimately depends on a node.
//
// The transitive closure of "is depended on by", counted per node. Where specificity
// asks how INFORMATIVE a dependency is, reach asks how far the consequences of touching
// something actually travel -- which is the blast radius the product is about, measured
// on the graph rather than on any particular change.
//
// Direct dependents are the wrong number for that. A package imported by one adapter
// that half the repository sits behind has a direct count of 1 and a reach of hundreds;
// treating it as a leaf puts the true core of the architecture out on the rim.
//
// Cycle-tolerant by construction: the fixed point is monotone, so an import cycle makes
// its members reach each other and converges rather than diverging.
#pragma once

#include "rgv/contract/Impact.h"
#include "rgv/model/GraphStore.h"

#include <unordered_map>

namespace rgv::analysis {

class ReachIndex {
public:
    // How many distinct nodes depend on this one, directly or through any chain.
    int dependents(const NodeId& id) const;

    // That count over the population, in [0, 1]. What the layout and the node size both
    // read, so "big" and "central" cannot end up saying different things.
    float fraction(const NodeId& id) const;

    // The largest reach any node has. The scale everything else is relative to.
    int widest() const { return widest_; }

    int  population() const { return population_; }
    bool empty() const { return counts_.empty(); }

private:
    friend ReachIndex build_reach(const GraphStore&, Level, const ImpactFilters&);

    std::unordered_map<NodeId, int> counts_;
    int                             population_ = 0;
    int                             widest_     = 0;
};

// Over the nodes at `level`, following only the edges the traversal policy in `filters`
// would follow -- the same edges specificity is scored on, so the two agree about what
// a dependency is.
ReachIndex build_reach(const GraphStore& store, Level level, const ImpactFilters& filters);

} // namespace rgv::analysis
