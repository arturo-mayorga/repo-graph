#include "rgv/analysis/Cycles.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace rgv::analysis {
namespace {

constexpr int kUnvisited = -1;

// Node ids interned to dense indices once, so the walk below is over vectors rather
// than hash lookups. Ids are interned in the order the edges name them; the result is
// sorted afterwards, so that order never reaches the report.
struct Graph {
    std::vector<NodeId>              ids;
    std::vector<std::vector<int>>    out;
    std::unordered_map<NodeId, int>  index;

    int intern(const NodeId& id) {
        auto [it, fresh] = index.try_emplace(id, static_cast<int>(ids.size()));
        if (fresh) {
            ids.push_back(id);
            out.emplace_back();
        }
        return it->second;
    }
};

} // namespace

// Tarjan's algorithm, with the recursion turned into an explicit stack of (node, next
// successor to consider). The recursive form is three lines shorter and overflows on
// any real repository.
std::vector<CycleGroup> find_cycles(const std::vector<std::pair<NodeId, NodeId>>& edges) {
    Graph g;
    for (const auto& [from, to] : edges) {
        if (from == to) continue;   // a module importing itself is not an entanglement
        const int a = g.intern(from);
        const int b = g.intern(to);
        g.out[static_cast<std::size_t>(a)].push_back(b);
    }

    const std::size_t n = g.ids.size();
    std::vector<int>  index(n, kUnvisited);
    std::vector<int>  low(n, 0);
    std::vector<char> on_stack(n, 0);
    std::vector<int>  component;   // Tarjan's stack of nodes awaiting a root
    std::vector<CycleGroup> out;

    int next_index = 0;
    // (node, how many of its successors have been walked)
    std::vector<std::pair<int, std::size_t>> work;

    for (std::size_t root = 0; root < n; ++root) {
        if (index[root] != kUnvisited) continue;
        work.push_back({static_cast<int>(root), 0});

        while (!work.empty()) {
            auto& [v, next] = work.back();
            const auto vi   = static_cast<std::size_t>(v);

            if (next == 0) {
                index[vi] = low[vi] = next_index++;
                component.push_back(v);
                on_stack[vi] = 1;
            }

            if (next < g.out[vi].size()) {
                const int  w  = g.out[vi][next++];
                const auto wi = static_cast<std::size_t>(w);
                if (index[wi] == kUnvisited) {
                    work.push_back({w, 0});   // descend; `v` resumes where it left off
                } else if (on_stack[wi]) {
                    low[vi] = std::min(low[vi], index[wi]);
                }
                continue;
            }

            // Every successor walked. `v` is a component root when nothing below it
            // reached anything older than itself.
            if (low[vi] == index[vi]) {
                CycleGroup group;
                for (;;) {
                    const int w  = component.back();
                    const auto wi = static_cast<std::size_t>(w);
                    component.pop_back();
                    on_stack[wi] = 0;
                    group.nodes.push_back(g.ids[wi]);
                    if (w == v) break;
                }
                // A single node is a component too -- and not a cycle, which is the
                // ordinary case and the reason this is the only filter that matters.
                if (group.nodes.size() > 1) {
                    std::sort(group.nodes.begin(), group.nodes.end());
                    out.push_back(std::move(group));
                }
            }

            work.pop_back();
            if (!work.empty()) {
                const auto parent = static_cast<std::size_t>(work.back().first);
                low[parent]       = std::min(low[parent], low[vi]);
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const CycleGroup& a, const CycleGroup& b) {
        return a.nodes < b.nodes;
    });
    return out;
}

} // namespace rgv::analysis
