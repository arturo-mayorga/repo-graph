#include "rgv/analysis/Specificity.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace rgv::analysis {
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

bool counts(const Edge& e, const ImpactFilters& f) {
    if (!e.active()) return false;
    if (!is_dependency_edge(e.kind)) return false;   // containment is not dependency
    if (std::find(f.edge_kinds.begin(), f.edge_kinds.end(), e.kind) == f.edge_kinds.end()) {
        return false;
    }
    if (!f.include_heuristic &&
        (e.confidence == Confidence::Heuristic || e.confidence == Confidence::Unresolved)) {
        return false;
    }
    return e.freshness != Freshness::Invalid;
}

} // namespace

int SpecificityIndex::dependents(const NodeId& id) const {
    auto it = df_.find(id);
    return it == df_.end() ? 0 : it->second;
}

float SpecificityIndex::specificity(const NodeId& id) const {
    auto it = scores_.find(id);
    return it == scores_.end() ? 1.0f : it->second;
}

float SpecificityIndex::dependent_fraction(const NodeId& id) const {
    if (population_ <= 0) return 0.0f;
    return static_cast<float>(dependents(id)) / static_cast<float>(population_);
}

SpecificityIndex build(const GraphStore& store, Level level, const ImpactFilters& filters) {
    const NodeKind   want = kind_for_level(level);
    SpecificityIndex index;

    std::vector<NodeId> population;
    for (const auto& [id, n] : store.nodes()) {
        if (n.kind == want) population.push_back(id);
    }
    index.population_ = static_cast<int>(population.size());
    if (index.population_ == 0) return index;

    for (const auto& id : population) {
        // Distinct dependents, not edge count: two files in the same package importing
        // the same thing is one architectural dependency, not two.
        std::unordered_set<NodeId> seen;
        for (const auto& eid : store.in_edges(id)) {
            const Edge* e = store.edge(eid);
            if (!e || !counts(*e, filters)) continue;
            const Node* dependent = store.node(e->from);
            if (!dependent || dependent->kind != want) continue;
            seen.insert(e->from);
        }
        index.df_[id] = static_cast<int>(seen.size());
    }

    const float n = static_cast<float>(index.population_);
    if (index.population_ <= 1) {
        // A single node carries no comparative information; calling it unspecific
        // would be an artefact of the population size, not a fact about the graph.
        for (const auto& id : population) index.scores_[id] = 1.0f;
        return index;
    }

    const float log_n = std::log(n);
    for (const auto& id : population) {
        const float df    = static_cast<float>(std::max(1, index.df_[id]));
        const float score = std::log(n / df) / log_n;
        index.scores_[id] = std::clamp(score, 0.0f, 1.0f);
    }
    return index;
}

float relevance(const GraphStore& store, const SpecificityIndex& index,
                const ImpactedNode& node) {
    // The seed is the change itself, not an inference from it. It is never discounted,
    // which is what stops a hub edit from being filtered away by its own low score.
    if (node.changed || node.min_distance == 0) return 1.0f;

    // No explanation to judge. Showing it is the safe default; hiding something the
    // backend could not explain would compound one gap with another.
    if (node.paths.empty()) return 1.0f;

    float best = 0.0f;
    for (const auto& path : node.paths) {
        // Weakest link: a single hop through a hub makes the whole chain unremarkable.
        float weakest = 1.0f;
        for (const auto& eid : path.edges) {
            const Edge* e = store.edge(eid);
            if (!e) continue;
            weakest = std::min(weakest, index.specificity(e->to));
        }
        // Several paths mean several independent reasons; the strongest one stands.
        best = std::max(best, weakest);
    }
    return best;
}

std::vector<HubAlert> hub_seeds(const GraphStore& store, const SpecificityIndex& index,
                                const ImpactResult& result, float threshold) {
    std::vector<HubAlert> alerts;
    if (index.empty()) return alerts;

    for (const auto& seed : result.seed_nodes) {
        const float spec = index.specificity(seed);
        if (spec > threshold) continue;

        HubAlert a;
        a.node        = seed;
        a.dependents  = index.dependents(seed);
        a.population  = index.population();
        a.specificity = spec;
        for (const auto& n : result.impacted_nodes) {
            if (n.min_distance > 0) ++a.reach;
        }
        a.reach_fraction = a.population > 0
                               ? static_cast<float>(a.reach) / static_cast<float>(a.population)
                               : 0.0f;
        alerts.push_back(a);
    }
    // Loudest first: the least specific hub is the biggest architectural event.
    std::sort(alerts.begin(), alerts.end(), [](const HubAlert& a, const HubAlert& b) {
        if (a.specificity != b.specificity) return a.specificity < b.specificity;
        return a.node < b.node;
    });
    return alerts;
}

} // namespace rgv::analysis
