// Architectural specificity: which dependencies actually carry information.
//
// Borrowed wholesale from inverse document frequency (Spärck Jones, 1972). In a
// corpus, a word appearing in nearly every document discriminates nothing; in a
// repository, a package that nearly everything depends on explains nothing. "auth
// depends on logger" is the architectural equivalent of the word "the".
//
//     specificity(n) = log(N / df(n)) / log(N)      in [0, 1]
//
// where df(n) is the number of distinct dependents and N the number of nodes at that
// level. A package everything depends on scores near 0; one with a single dependent
// scores 1.
//
// IMPORTANT: this measures how informative a node is *as an explanation*, not how
// important it is. When a hub is the thing that CHANGED, its blast radius is genuinely
// the whole repository and that is the loudest possible signal -- see hub_seeds().
// Specificity is therefore a weight on edges and on ranking, never a filter on seeds.
#pragma once

#include "rgv/contract/Impact.h"
#include "rgv/model/GraphStore.h"

#include <unordered_map>
#include <vector>

namespace rgv::analysis {

class SpecificityIndex {
public:
    // Number of distinct dependents. The "document frequency" of the analogy.
    int dependents(const NodeId& id) const;

    // In [0, 1]. Unknown nodes score 1: never discount something you did not measure.
    float specificity(const NodeId& id) const;

    // How many nodes were in the population, i.e. N.
    int population() const { return population_; }

    bool empty() const { return scores_.empty(); }

    // Fraction of the population that depends on this node. What the UI actually
    // shows a human, because "6 of 8 packages" reads better than "specificity 0.12".
    float dependent_fraction(const NodeId& id) const;

private:
    friend SpecificityIndex build(const GraphStore&, Level, const ImpactFilters&);

    std::unordered_map<NodeId, int>   df_;
    std::unordered_map<NodeId, float> scores_;
    int                               population_ = 0;
};

// Builds the index over nodes at `level`, counting only edges the traversal policy in
// `filters` would actually follow. Structural edges never count: containment is not
// dependency, and letting it in would make every file look like a hub.
SpecificityIndex build(const GraphStore& store, Level level, const ImpactFilters& filters);

// How much an impacted node's presence in the blast radius actually tells you.
//
// The weakest link along its shortest explanation: one hop through a hub makes the
// whole chain unremarkable, because "everything depends on the hub" was already known.
// Seeds score 1 by definition -- they are the change, not an inference from it.
float relevance(const GraphStore& store, const SpecificityIndex& index,
                const ImpactedNode& node);

// A change to a node that most of the repository depends on.
struct HubAlert {
    NodeId node;
    int    dependents        = 0;
    int    population        = 0;
    float  specificity       = 1.0f;
    int    reach             = 0;     // nodes in the blast radius
    float  reach_fraction    = 0.0f;  // reach / population
};

// Seeds of `result` whose specificity is at or below `threshold`. These are the
// changes worth shouting about: the radius is huge and every individual entry in it
// is unsurprising, so the headline has to be the hub itself.
std::vector<HubAlert> hub_seeds(const GraphStore& store, const SpecificityIndex& index,
                                const ImpactResult& result, float threshold);

// Default cut-off for calling something a hub. At N=8, this is roughly "a third or
// more of the repository depends on it".
inline constexpr float kHubThreshold = 0.35f;

} // namespace rgv::analysis
