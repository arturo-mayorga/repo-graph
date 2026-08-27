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

// Radial filesystem layout, in the spirit of Gource.
//
// Three ideas carry the look. A directory is a disc whose radius comes from how many
// files it directly holds, so size means something at a glance. Those files sit on the
// disc's rim, orbiting the thing that owns them. Child directories splay outward from
// their parent, packed into concentric shells inside the parent's wedge.
//
// Gource pushes nodes apart with a force simulation. This does it by construction:
// wedges are disjoint, and shells fill outward so siblings never land on one another.
// Same result -- nothing overlaps, branches fan out organically -- but deterministic,
// with nothing to settle and no drift between runs.
void LayoutSystem::radial_tree(ecs::World& world) {
    auto& reg = world.registry;

    // The containment edges the filesystem view synthesises from parent links.
    std::unordered_map<std::uint32_t, std::vector<entt::entity>> kids;
    std::unordered_map<std::uint32_t, std::uint32_t>             parent;
    for (auto [ent, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind != EdgeKind::Contains) continue;
        kids[to_raw(ends.to)].push_back(ends.from);
        parent[to_raw(ends.from)] = to_raw(ends.to);
    }

    auto is_file = [&](entt::entity e) {
        const auto* ref = reg.try_get<ecs::NodeRef>(e);
        return ref && ref->kind == NodeKind::File;
    };
    auto label_of = [&](entt::entity e) {
        const auto* l = reg.try_get<ecs::Label>(e);
        return l ? l->text : std::string{};
    };

    // Stable ordering, so the same repository always draws the same way.
    for (auto& [k, v] : kids) {
        std::sort(v.begin(), v.end(), [&](entt::entity a, entt::entity b) {
            const bool fa = is_file(a), fb = is_file(b);
            if (fa != fb) return !fa;                 // directories first, then files
            return label_of(a) < label_of(b);
        });
    }

    std::vector<entt::entity> roots;
    for (auto [ent, ref] : reg.view<const ecs::NodeRef>().each()) {
        if (!parent.count(to_raw(ent))) roots.push_back(ent);
    }
    std::sort(roots.begin(), roots.end(),
              [&](entt::entity a, entt::entity b) { return label_of(a) < label_of(b); });

    // -- bottom-up: disc radius, subtree weight, and how far each subtree reaches ----
    std::unordered_map<std::uint32_t, float> ring, extent;
    std::unordered_map<std::uint32_t, float> weight;   // leaf count, for sector sizing

    struct Visit { entt::entity node; std::size_t next; };
    std::vector<Visit> stack;

    for (auto root : roots) {
        stack.push_back({root, 0});
        while (!stack.empty()) {
            Visit& v  = stack.back();
            auto&  ch = kids[to_raw(v.node)];
            if (v.next < ch.size()) {
                stack.push_back({ch[v.next++], 0});
                continue;
            }
            const std::uint32_t id = to_raw(v.node);

            if (is_file(v.node)) {
                ring[id]   = params_.file_radius;
                extent[id] = params_.file_radius;
                weight[id] = 1.0f;
            } else {
                int   files = 0;
                float w     = 0.0f;
                for (auto c : ch) {
                    if (is_file(c)) ++files;
                    w += weight[to_raw(c)];
                }
                weight[id] = std::max(1.0f, w);

                // The rim has to be long enough to seat every file without crowding,
                // which is what makes a directory's size read as its file count.
                const float per_file = 2.0f * params_.file_radius + params_.file_gap;
                const float needed   = static_cast<float>(files) * per_file /
                                     (2.0f * 3.14159265f);
                ring[id] = std::max(params_.min_dir_ring, needed);

                float reach = ring[id] + params_.file_radius;
                for (auto c : ch) {
                    if (is_file(c)) continue;
                    // Placed just clear of this ring, so its subtree reaches this far.
                    const float d = ring[id] + params_.dir_gap + extent[to_raw(c)];
                    reach         = std::max(reach, d + extent[to_raw(c)]);
                }
                extent[id] = reach;
            }
            stack.pop_back();
        }
    }

    // -- top-down: place ------------------------------------------------------------
    struct Place {
        entt::entity node;
        Vec2         pos;
        float        angle;   // bisector of this node's wedge
        float        span;    // angular width available to it
        int          depth;
    };
    std::vector<Place> queue;

    // Normally a single root: the repository. Several only if the snapshot has no
    // repository node, in which case they share the circle.
    if (roots.size() == 1) {
        queue.push_back({roots[0], Vec2{0.0f, 0.0f}, 0.0f, 6.2831853f, 0});
    } else {
        float span_total = 0.0f;
        for (auto r : roots) span_total += 2.0f * extent[to_raw(r)] + params_.dir_gap;
        const float ring_r = std::max(1.0f, span_total / 6.2831853f);

        float a = 0.0f;
        for (auto r : roots) {
            const float need  = 2.0f * extent[to_raw(r)] + params_.dir_gap;
            const float share = 6.2831853f * need / span_total;
            const float mid   = a + share * 0.5f;
            a += share;
            queue.push_back({r, Vec2{std::cos(mid) * ring_r, std::sin(mid) * ring_r}, mid,
                             share, 1});
        }
    }

    while (!queue.empty()) {
        const Place p = queue.back();
        queue.pop_back();
        const std::uint32_t id = to_raw(p.node);

        reg.emplace_or_replace<ecs::LayoutTarget>(p.node, ecs::LayoutTarget{p.pos});
        if (!reg.all_of<ecs::Position>(p.node)) {
            reg.emplace<ecs::Position>(p.node, ecs::Position{p.pos});
        }
        reg.emplace_or_replace<ecs::Depth>(p.node, ecs::Depth{p.depth});
        reg.emplace_or_replace<ecs::Disc>(p.node, ecs::Disc{ring[id], extent[id]});

        auto& ch = kids[id];
        if (ch.empty()) continue;

        std::vector<entt::entity> files, dirs;
        for (auto c : ch) (is_file(c) ? files : dirs).push_back(c);

        // Files ring the directory that owns them. They take the whole circle rather
        // than the node's wedge -- that halo is the shape Gource is recognisable by,
        // and the ring is small enough to stay inside the subtree's own extent.
        for (std::size_t i = 0; i < files.size(); ++i) {
            const float a = 6.2831853f * static_cast<float>(i) /
                            static_cast<float>(files.size());
            const Vec2 at{p.pos.x + std::cos(a) * ring[id], p.pos.y + std::sin(a) * ring[id]};
            reg.emplace_or_replace<ecs::LayoutTarget>(files[i], ecs::LayoutTarget{at});
            if (!reg.all_of<ecs::Position>(files[i])) {
                reg.emplace<ecs::Position>(files[i], ecs::Position{at});
            }
            reg.emplace_or_replace<ecs::Depth>(files[i], ecs::Depth{p.depth + 1});
            reg.emplace_or_replace<ecs::Disc>(
                files[i], ecs::Disc{params_.file_radius, params_.file_radius});
        }

        if (dirs.empty()) continue;

        // Children are packed into concentric shells inside the wedge, not spread
        // evenly around one circle.
        //
        // A single ring is what a pure sector layout gives you, and at 240 siblings it
        // degenerates: every child gets the same tiny wedge, so every child is pushed
        // to the same radius and the tree becomes a perfect annulus with a void in the
        // middle. Gource avoids that because its forces let branches bunch at
        // different distances. Filling shells outward reproduces that -- a big flat
        // directory becomes a packed disc rather than a hoop.
        std::sort(dirs.begin(), dirs.end(), [&](entt::entity a, entt::entity b) {
            return extent[to_raw(a)] > extent[to_raw(b)];
        });

        float       r = ring[id] + params_.dir_gap;
        std::size_t i = 0;
        while (i < dirs.size()) {
            // Far enough out to clear the parent's file ring and seat the largest
            // child in this shell.
            r = std::max(r, ring[id] + params_.dir_gap + extent[to_raw(dirs[i])]);

            const float arc = r * p.span;
            float       used = 0.0f, tallest = 0.0f;
            std::size_t j = i;
            while (j < dirs.size()) {
                const float need = 2.0f * extent[to_raw(dirs[j])] + params_.dir_gap;
                if (j > i && used + need > arc) break;   // always seat at least one
                used += need;
                tallest = std::max(tallest, extent[to_raw(dirs[j])]);
                ++j;
            }

            float a = p.angle - p.span * 0.5f;
            for (std::size_t k = i; k < j; ++k) {
                const std::uint32_t cid   = to_raw(dirs[k]);
                const float need  = 2.0f * extent[cid] + params_.dir_gap;
                const float share = p.span * need / std::max(1.0f, used);
                const float mid   = a + share * 0.5f;
                a += share;

                queue.push_back({dirs[k],
                                 Vec2{p.pos.x + std::cos(mid) * r, p.pos.y + std::sin(mid) * r},
                                 mid, share, p.depth + 1});
            }

            r += 2.0f * tallest + params_.dir_gap;
            i = j;
        }
    }
    energy_ = 1e9f;
}

void LayoutSystem::reset(ecs::World& world) {
    tree_mode_ = world.resource<ecs::ViewSettings>().mode == ecs::ViewMode::Filesystem;
    if (tree_mode_) {
        radial_tree(world);
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
