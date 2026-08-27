#include "rgv/systems/LayoutSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace rgv::systems {
namespace {

std::uint32_t to_raw(entt::entity e) { return static_cast<std::uint32_t>(e); }

} // namespace

void LayoutSystem::assign_depths(ecs::World& world) {
    auto& reg = world.registry;

    // Adjacency over what is on screen, not over the whole store: depth has to
    // describe the picture the user is actually looking at.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> deps;
    for (auto [ent, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) continue;
        deps[to_raw(ends.from)].push_back(to_raw(ends.to));
    }

    std::unordered_map<std::uint32_t, int> depth;
    std::unordered_map<std::uint32_t, int> state;   // 0 unvisited, 1 open, 2 done
    std::vector<std::uint32_t>             stack;

    // Longest path to a sink. Iterative because a deep monorepo would blow a
    // recursive stack, and cycle-tolerant because import graphs really do cycle.
    for (auto [ent, ref] : reg.view<const ecs::NodeRef>().each()) {
        const std::uint32_t root = to_raw(ent);
        if (state[root] == 2) continue;
        stack.push_back(root);
        while (!stack.empty()) {
            const std::uint32_t cur = stack.back();
            if (state[cur] == 0) {
                state[cur]  = 1;
                bool pushed = false;
                for (auto next : deps[cur]) {
                    if (state[next] == 0) { stack.push_back(next); pushed = true; }
                }
                if (pushed) continue;
            }
            stack.pop_back();
            if (state[cur] == 2) continue;
            int d = 0;
            for (auto next : deps[cur]) {
                // A back-edge into an open node is a cycle. Treating it as depth 0
                // keeps the layout finite instead of diverging.
                if (state[next] == 2) d = std::max(d, depth[next] + 1);
            }
            depth[cur] = d;
            state[cur] = 2;
        }
    }

    depth_span_ = 1;
    for (auto [ent, ref] : reg.view<const ecs::NodeRef>().each()) {
        const int d = depth[to_raw(ent)];
        reg.emplace_or_replace<ecs::Depth>(ent, ecs::Depth{d});
        depth_span_ = std::max(depth_span_, d + 1);
    }
}

void LayoutSystem::order_and_place(ecs::World& world) {
    auto& reg = world.registry;

    std::vector<std::vector<entt::entity>> layers(static_cast<std::size_t>(depth_span_));
    for (auto [ent, ref, depth] : reg.view<const ecs::NodeRef, const ecs::Depth>().each()) {
        layers[static_cast<std::size_t>(depth.value)].push_back(ent);
    }

    // Seed each row from where its nodes already are, so re-running layout after a
    // graph change preserves the arrangement the user has been looking at. Nodes with
    // no position yet sort last, deterministically.
    for (auto& row : layers) {
        std::sort(row.begin(), row.end(), [&](entt::entity a, entt::entity b) {
            const auto* pa = reg.try_get<ecs::Position>(a);
            const auto* pb = reg.try_get<ecs::Position>(b);
            if (pa && pb) return pa->p.x < pb->p.x;
            if (pa != pb) return pa != nullptr;
            const auto* la = reg.try_get<ecs::Label>(a);
            const auto* lb = reg.try_get<ecs::Label>(b);
            return (la ? la->text : "") < (lb ? lb->text : "");
        });
    }

    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> up, down;
    for (auto [ent, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) continue;
        up[to_raw(ends.from)].push_back(to_raw(ends.to));     // toward lower depth
        down[to_raw(ends.to)].push_back(to_raw(ends.from));   // toward higher depth
    }

    // Barycentre sweeps. Each node drifts toward the average index of its neighbours
    // in the adjacent row; alternating direction converges on few crossings.
    auto index_of = [&](const std::vector<entt::entity>& row) {
        std::unordered_map<std::uint32_t, float> idx;
        for (std::size_t i = 0; i < row.size(); ++i) idx[to_raw(row[i])] = static_cast<float>(i);
        return idx;
    };

    for (int sweep = 0; sweep < params_.sweeps; ++sweep) {
        const bool downward = (sweep % 2) == 0;
        for (std::size_t li = 0; li < layers.size(); ++li) {
            const std::size_t l = downward ? li : layers.size() - 1 - li;
            const std::size_t ref_layer_idx = downward ? (l == 0 ? 0 : l - 1)
                                                       : std::min(l + 1, layers.size() - 1);
            if (ref_layer_idx == l) continue;

            const auto ref_index = index_of(layers[ref_layer_idx]);
            const auto& adj      = downward ? up : down;

            std::vector<std::pair<float, entt::entity>> keyed;
            keyed.reserve(layers[l].size());
            for (std::size_t i = 0; i < layers[l].size(); ++i) {
                const entt::entity e = layers[l][i];
                float sum = 0.0f;
                int   n   = 0;
                auto  it  = adj.find(to_raw(e));
                if (it != adj.end()) {
                    for (auto nb : it->second) {
                        auto f = ref_index.find(nb);
                        if (f != ref_index.end()) { sum += f->second; ++n; }
                    }
                }
                // No neighbour in the reference row: hold position rather than
                // collapsing to zero and dragging unrelated nodes across the picture.
                keyed.emplace_back(n ? sum / static_cast<float>(n) : static_cast<float>(i), e);
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < keyed.size(); ++i) layers[l][i] = keyed[i].second;
        }
    }

    // Row widths decide the vertical spacing. A monorepo with 240 packages over six
    // layers packs eighty nodes into a row, and at the nominal gap the drawing becomes
    // a ribbon thousands of units wide and a few hundred tall -- fitting it wastes the
    // whole viewport. Stretching the layers to match the viewport's aspect keeps the
    // graph filling the space it is given, at any scale.
    auto row_width = [&](const std::vector<entt::entity>& row) {
        float total = 0.0f;
        for (auto e : row) {
            const auto* ext = reg.try_get<ecs::Extent>(e);
            total += (ext ? ext->half.x * 2.0f : 100.0f) + params_.node_gap;
        }
        return row.empty() ? 0.0f : total - params_.node_gap;
    };

    float widest = 0.0f;
    for (const auto& row : layers) widest = std::max(widest, row_width(row));

    const Vec2  free   = world.resource<ecs::Viewport>().free_size;
    const float aspect = (free.x > 1.0f && free.y > 1.0f) ? free.x / free.y : 1.6f;
    const int   gaps   = std::max(1, static_cast<int>(layers.size()) - 1);
    const float layer_gap =
        std::clamp(widest / aspect / static_cast<float>(gaps),
                   params_.layer_gap, params_.layer_gap * 12.0f);

    // Place: rows are centred on x = 0, and depth 0 sits at the bottom so impact
    // reads upward, the way the spec draws it.
    for (std::size_t l = 0; l < layers.size(); ++l) {
        const auto& row   = layers[l];
        const float total = row_width(row);

        const float y = -static_cast<float>(l) * layer_gap;
        float       x = -total * 0.5f;
        for (auto e : row) {
            const auto* ext = reg.try_get<ecs::Extent>(e);
            const float w   = ext ? ext->half.x * 2.0f : 100.0f;
            const Vec2  target{x + w * 0.5f, y};
            x += w + params_.node_gap;

            if (reg.all_of<ecs::Pinned>(e)) {
                // A pinned node keeps its slot in the ordering but not its target:
                // the user placed it, so layout stops arguing.
                if (const auto* p = reg.try_get<ecs::Position>(e)) {
                    reg.emplace_or_replace<ecs::LayoutTarget>(e, ecs::LayoutTarget{p->p});
                    continue;
                }
            }
            reg.emplace_or_replace<ecs::LayoutTarget>(e, ecs::LayoutTarget{target});
            if (!reg.all_of<ecs::Position>(e)) reg.emplace<ecs::Position>(e, ecs::Position{target});
        }
    }
    energy_ = 1e9f;
}

void LayoutSystem::tidy_tree(ecs::World& world) {
    auto& reg = world.registry;

    std::unordered_map<std::uint32_t, std::vector<entt::entity>> kids;
    std::unordered_map<std::uint32_t, std::uint32_t>             parent;
    for (auto [ent, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind != EdgeKind::Contains) continue;
        kids[to_raw(ends.to)].push_back(ends.from);
        parent[to_raw(ends.from)] = to_raw(ends.to);
    }

    auto by_label = [&](entt::entity a, entt::entity b) {
        const auto* la = reg.try_get<ecs::Label>(a);
        const auto* lb = reg.try_get<ecs::Label>(b);
        return (la ? la->text : "") < (lb ? lb->text : "");
    };
    for (auto& [k, v] : kids) std::sort(v.begin(), v.end(), by_label);

    std::vector<entt::entity> roots;
    for (auto [ent, ref] : reg.view<const ecs::NodeRef>().each()) {
        if (!parent.count(to_raw(ent))) roots.push_back(ent);
    }
    std::sort(roots.begin(), roots.end(), by_label);

    float       slot = 0.0f;
    const float col = 210.0f, row = 42.0f;

    // Explicit stack rather than recursion: a vendored dependency tree gets deep.
    struct Frame { entt::entity node; int depth; std::size_t next; };
    std::vector<Frame> stack;

    for (auto root : roots) {
        stack.push_back({root, 0, 0});
        while (!stack.empty()) {
            Frame& f  = stack.back();
            auto&  ch = kids[to_raw(f.node)];
            if (f.next < ch.size()) {
                stack.push_back({ch[f.next++], f.depth + 1, 0});
                continue;
            }
            float y;
            if (ch.empty()) {
                y = slot;
                slot += row;
            } else {
                // Centre a parent over the span its children occupy.
                float lo = 1e30f, hi = -1e30f;
                for (auto c : ch) {
                    if (const auto* t = reg.try_get<ecs::LayoutTarget>(c)) {
                        lo = std::min(lo, t->p.y);
                        hi = std::max(hi, t->p.y);
                    }
                }
                y = (lo + hi) * 0.5f;
            }
            const Vec2 target{static_cast<float>(f.depth) * col, y};
            reg.emplace_or_replace<ecs::LayoutTarget>(f.node, ecs::LayoutTarget{target});
            if (!reg.all_of<ecs::Position>(f.node)) reg.emplace<ecs::Position>(f.node, ecs::Position{target});
            reg.emplace_or_replace<ecs::Depth>(f.node, ecs::Depth{f.depth});
            stack.pop_back();
        }
        slot += row;   // gap between top-level trees
    }
    energy_ = 1e9f;
}

void LayoutSystem::reset(ecs::World& world) {
    tree_mode_ = world.resource<ecs::ViewSettings>().mode == ecs::ViewMode::Filesystem;
    if (tree_mode_) {
        tidy_tree(world);
    } else {
        assign_depths(world);
        order_and_place(world);
    }
}

void LayoutSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto& requests = world.resource<ecs::SceneRequests>();
    if (requests.relayout) {
        reset(world);
        requests.relayout = false;
    }

    auto&       stats = world.resource<ecs::SceneStats>();
    const auto& view  = world.resource<ecs::ViewSettings>();
    if (!view.layout_running) {
        stats.layout_energy  = energy_;
        stats.layout_settled = energy_ < 0.5f;
        return;
    }

    auto&       reg = world.registry;
    const float t   = params_.ease <= 0.0f
                          ? 1.0f
                          : std::clamp(frame.dt * params_.ease, 0.0f, 1.0f);

    float worst = 0.0f;
    for (auto [ent, pos, target] :
         reg.view<ecs::Position, const ecs::LayoutTarget>().each()) {
        if (reg.all_of<ecs::Pinned>(ent)) continue;
        const Vec2 d = target.p - pos.p;
        worst        = std::max(worst, length(d));
        pos.p += d * t;
    }
    energy_ = worst;

    stats.layout_energy  = energy_;
    stats.layout_settled = energy_ < 0.5f;
}

} // namespace rgv::systems
