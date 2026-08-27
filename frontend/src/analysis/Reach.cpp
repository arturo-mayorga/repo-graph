#include "rgv/analysis/Reach.h"

#include <algorithm>
#include <cstdint>
#include <vector>

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

int ReachIndex::dependents(const NodeId& id) const {
    auto it = counts_.find(id);
    return it == counts_.end() ? 0 : it->second;
}

float ReachIndex::fraction(const NodeId& id) const {
    if (population_ <= 0) return 0.0f;
    return static_cast<float>(dependents(id)) / static_cast<float>(population_);
}

ReachIndex build_reach(const GraphStore& store, Level level, const ImpactFilters& filters) {
    const NodeKind want = kind_for_level(level);
    ReachIndex     index;

    std::vector<NodeId>                     population;
    std::unordered_map<NodeId, std::size_t> slot;
    for (const auto& [id, n] : store.nodes()) {
        if (n.kind != want) continue;
        slot[id] = population.size();
        population.push_back(id);
    }
    index.population_ = static_cast<int>(population.size());
    if (population.empty()) return index;

    const std::size_t n     = population.size();
    const std::size_t words = (n + 63) / 64;

    // dependency slot -> the dependents feeding into it. Built once; the fixed point
    // below sweeps it repeatedly.
    std::vector<std::vector<std::size_t>> feeders(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto& eid : store.in_edges(population[i])) {
            const Edge* e = store.edge(eid);
            if (!e || !counts(*e, filters)) continue;
            auto it = slot.find(e->from);
            if (it == slot.end() || it->second == i) continue;
            feeders[i].push_back(it->second);
        }
        std::sort(feeders[i].begin(), feeders[i].end());
        feeders[i].erase(std::unique(feeders[i].begin(), feeders[i].end()), feeders[i].end());
    }

    // reach[i] = the set of nodes that depend on i. Propagated as bitsets rather than a
    // traversal per node: a search from every node is quadratic on a wide graph, while
    // this is a handful of passes over the edges, each a word-at-a-time union.
    std::vector<std::uint64_t> reach(n * words, 0);
    auto bit = [&](std::size_t owner, std::size_t member) {
        reach[owner * words + member / 64] |= (std::uint64_t{1} << (member % 64));
    };
    for (std::size_t i = 0; i < n; ++i) {
        for (auto f : feeders[i]) bit(i, f);
    }

    // Monotone, so it terminates: every pass only ever sets bits, and there are finitely
    // many. The bound is the longest dependency chain; the guard is for safety, not for
    // convergence.
    for (std::size_t pass = 0; pass < n + 1; ++pass) {
        bool grew = false;
        for (std::size_t i = 0; i < n; ++i) {
            for (auto f : feeders[i]) {
                // Anything depending on a dependent of i depends on i.
                for (std::size_t w = 0; w < words; ++w) {
                    const std::uint64_t add = reach[f * words + w] & ~reach[i * words + w];
                    if (!add) continue;
                    reach[i * words + w] |= add;
                    grew = true;
                }
            }
        }
        if (!grew) break;
    }

    for (std::size_t i = 0; i < n; ++i) {
        int c = 0;
        for (std::size_t w = 0; w < words; ++w) {
            c += __builtin_popcountll(reach[i * words + w]);
        }
        // A cycle makes a node reach itself. It does not depend on itself in any sense
        // the user cares about, so it is not counted.
        if (reach[i * words + i / 64] & (std::uint64_t{1} << (i % 64))) --c;
        index.counts_[population[i]] = c;
        index.widest_                = std::max(index.widest_, c);
    }
    return index;
}

} // namespace rgv::analysis
