#include "Impact.h"

#include <algorithm>
#include <deque>
#include <unordered_map>

namespace rgv::watch {

std::vector<ImpactHit> reverse_reach(const std::vector<GraphEdge>&   edges,
                                     const std::vector<std::string>& seeds,
                                     long                            baseline_generation,
                                     int                             max_depth) {
    // dependency -> the edges that point at it, i.e. its dependents.
    std::unordered_map<std::string, std::vector<const GraphEdge*>> dependents;
    for (const auto& e : edges) {
        if (e.heuristic) continue;
        dependents[e.to].push_back(&e);
    }

    struct Visit {
        int              distance = 0;
        const GraphEdge* via      = nullptr;   // edge from this node toward the seed
    };
    std::unordered_map<std::string, Visit> seen;
    std::deque<std::string>                queue;

    for (const auto& s : seeds) {
        if (seen.emplace(s, Visit{0, nullptr}).second) queue.push_back(s);
    }
    while (!queue.empty()) {
        const std::string node = queue.front();
        queue.pop_front();
        const int d = seen[node].distance;
        if (d >= max_depth) continue;
        auto it = dependents.find(node);
        if (it == dependents.end()) continue;
        for (const GraphEdge* e : it->second) {
            if (seen.emplace(e->from, Visit{d + 1, e}).second) queue.push_back(e->from);
        }
    }

    std::vector<ImpactHit> out;
    out.reserve(seen.size());
    for (const auto& [node, v] : seen) {
        ImpactHit h;
        h.node     = node;
        h.distance = v.distance;
        for (const GraphEdge* e = v.via; e != nullptr; e = seen[e->to].via) {
            h.path.push_back(e->id);
            if (e->valid_from > baseline_generation) h.dependency_added = true;
        }
        out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(),
              [](const ImpactHit& a, const ImpactHit& b) { return a.node < b.node; });
    return out;
}

} // namespace rgv::watch
