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
// Three ideas carry the look. A directory is a small disc that grows gently with the
// files it holds, so size means something without any one node dwarfing its neighbours.
// Those files orbit it at a distance -- clear of the disc, not sitting on its edge.
// Child subtrees pack into concentric shells around their parent and fan outward.
//
// Gource pushes nodes apart with a force simulation. This does it by construction:
// every subtree is laid out in its own local frame first, so its exact enclosing radius
// is known, and a parent then packs those subtrees as rigid discs. Nothing overlaps,
// nothing has to settle, and the same repository always draws identically.
//
// Laying out bottom-up is what makes that exact. Estimating a subtree's reach top-down
// and hoping the estimate holds is how radial layouts end up with subtrees quietly
// growing through one another.
void LayoutSystem::radial_tree(ecs::World& world) {
    auto& reg = world.registry;

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

    // A subtree, laid out in its own frame: every node's offset from the subtree root,
    // and the exact radius of the disc that encloses all of it.
    struct Sub {
        float                                        radius = 0.0f;
        std::vector<std::pair<entt::entity, Vec2>>   nodes;
    };

    // Deterministic per-node phase, so single-child chains do not all point the same
    // way. Gource looks organic partly because nothing lines up.
    auto phase_of = [](entt::entity e) {
        std::uint32_t h = 2166136261u ^ static_cast<std::uint32_t>(e);
        h *= 16777619u;
        return 6.2831853f * static_cast<float>(h % 1024u) / 1024.0f;
    };

    auto draw_radius = [&](std::size_t files) {
        return std::min(params_.dir_radius_max,
                        params_.dir_radius +
                            params_.dir_radius_per * std::sqrt(static_cast<float>(files)));
    };

    // Explicit stack rather than recursion: a vendored dependency tree gets deep.
    struct Frame { entt::entity node; std::size_t next; std::vector<Sub> done; };
    std::vector<Frame>                            stack;
    std::unordered_map<std::uint32_t, Sub>        built;

    auto build = [&](entt::entity root) {
        stack.push_back({root, 0, {}});
        while (!stack.empty()) {
            Frame& f  = stack.back();
            auto&  ch = kids[to_raw(f.node)];

            // Descend into child directories first; files need no layout of their own.
            bool descended = false;
            while (f.next < ch.size()) {
                const entt::entity c = ch[f.next];
                if (is_file(c)) { ++f.next; continue; }
                ++f.next;
                stack.push_back({c, 0, {}});
                descended = true;
                break;
            }
            if (descended) continue;

            Sub out;
            if (is_file(f.node)) {
                out.radius = params_.file_radius;
                out.nodes.push_back({f.node, Vec2{0.0f, 0.0f}});
            } else {
                std::size_t files = 0;
                for (auto c : ch) {
                    if (is_file(c)) ++files;
                }
                const float draw = draw_radius(files);
                out.nodes.push_back({f.node, Vec2{0.0f, 0.0f}});

                // Files orbit clear of the disc, on a ring long enough to seat them
                // all without crowding. The halo's size is the file count made visible.
                float hull = draw;
                if (files > 0) {
                    const float per   = 2.0f * params_.file_radius + params_.file_gap;
                    const float seat  = static_cast<float>(files) * per / 6.2831853f;
                    const float orbit = std::max(draw + params_.file_radius + params_.orbit_gap,
                                                 seat);
                    const float base  = phase_of(f.node);
                    std::size_t i     = 0;
                    for (auto c : ch) {
                        if (!is_file(c)) continue;
                        const float a = base + 6.2831853f * static_cast<float>(i++) /
                                                   static_cast<float>(files);
                        out.nodes.push_back(
                            {c, Vec2{std::cos(a) * orbit, std::sin(a) * orbit}});
                    }
                    hull = orbit + params_.file_radius;
                }
                out.radius = hull;

                // Child subtrees, largest first, packed into shells that fill outward.
                // One shell would put every sibling at the same radius; at 240 siblings
                // that degenerates into a ring with a void in the middle.
                std::vector<Sub>& subs = f.done;
                std::sort(subs.begin(), subs.end(),
                          [](const Sub& a, const Sub& b) { return a.radius > b.radius; });

                // Where the first ring sits decides whether this looks like a flower
                // or like a comet. Starting as tight as possible seats a few children
                // and flings the rest into a distant second shell; starting wide
                // enough to seat everything degenerates into an annulus once there are
                // hundreds. So: seat them all on one ring when that ring is a sane size
                // relative to the children, and spill into shells only when it is not.
                float need_all = 0.0f, largest = 0.0f;
                for (const auto& sub : subs) {
                    need_all += 2.0f * sub.radius + params_.dir_gap;
                    largest = std::max(largest, sub.radius);
                }
                const float one_ring = need_all * 1.02f / 6.2831853f;
                const float widest   = params_.shell_spread * std::max(largest, 1.0f);

                float       r = std::max(hull + params_.dir_gap + largest,
                                         std::min(one_ring, widest));
                std::size_t i     = 0;
                const float base  = phase_of(f.node) * 0.5f;
                int         shell = 0;
                while (i < subs.size()) {
                    r = std::max(r, hull + params_.dir_gap + subs[i].radius);

                    const float arc  = r * 6.2831853f;
                    float       used = 0.0f, tallest = 0.0f;
                    std::size_t j    = i;
                    while (j < subs.size()) {
                        const float need = 2.0f * subs[j].radius + params_.dir_gap;
                        if (j > i && used + need > arc) break;   // always seat at least one
                        used += need;
                        tallest = std::max(tallest, subs[j].radius);
                        ++j;
                    }

                    // Each shell is rotated off the last, so successive rings do not
                    // line up into spokes radiating from the parent.
                    float a = base + 0.5f * static_cast<float>(shell++);
                    for (std::size_t k = i; k < j; ++k) {
                        const float need  = 2.0f * subs[k].radius + params_.dir_gap;
                        const float share = 6.2831853f * need / std::max(1.0f, used);
                        const float mid   = a + share * 0.5f;
                        a += share;

                        const Vec2 at{std::cos(mid) * r, std::sin(mid) * r};
                        for (const auto& [e, off] : subs[k].nodes) {
                            out.nodes.push_back({e, at + off});
                        }
                        out.radius = std::max(out.radius, r + subs[k].radius);
                    }
                    r += 2.0f * tallest + params_.dir_gap;
                    i = j;
                }
            }

            stack.pop_back();
            if (stack.empty()) built[to_raw(f.node)] = std::move(out);
            else stack.back().done.push_back(std::move(out));
        }
    };

    std::vector<entt::entity> roots;
    for (auto [ent, ref] : reg.view<const ecs::NodeRef>().each()) {
        if (!parent.count(to_raw(ent))) roots.push_back(ent);
    }
    std::sort(roots.begin(), roots.end(),
              [&](entt::entity a, entt::entity b) { return label_of(a) < label_of(b); });

    // -- commit -------------------------------------------------------------------
    auto commit = [&](const Sub& sub, Vec2 origin) {
        for (const auto& [e, off] : sub.nodes) {
            const Vec2 at = origin + off;
            reg.emplace_or_replace<ecs::LayoutTarget>(e, ecs::LayoutTarget{at});
            if (!reg.all_of<ecs::Position>(e)) reg.emplace<ecs::Position>(e, ecs::Position{at});

            std::size_t files = 0;
            for (auto c : kids[to_raw(e)]) {
                if (is_file(c)) ++files;
            }
            const float radius = is_file(e) ? params_.file_radius : draw_radius(files);
            reg.emplace_or_replace<ecs::Disc>(e, ecs::Disc{radius, radius});
        }
    };

    if (roots.size() == 1) {
        build(roots[0]);
        commit(built[to_raw(roots[0])], Vec2{0.0f, 0.0f});
    } else {
        // Normally a single root: the repository. Several only if the snapshot has no
        // repository node, in which case they share a ring.
        float total = 0.0f;
        for (auto r : roots) {
            build(r);
            total += 2.0f * built[to_raw(r)].radius + params_.dir_gap;
        }
        const float ring = std::max(1.0f, total / 6.2831853f);
        float       a    = 0.0f;
        for (auto r : roots) {
            const Sub&  sub   = built[to_raw(r)];
            const float need  = 2.0f * sub.radius + params_.dir_gap;
            const float share = 6.2831853f * need / std::max(1.0f, total);
            const float mid   = a + share * 0.5f;
            a += share;
            commit(sub, Vec2{std::cos(mid) * ring, std::sin(mid) * ring});
        }
    }

    // Depth, for anything that wants it.
    for (auto [ent, ref] : reg.view<const ecs::NodeRef>().each()) {
        int           d   = 0;
        std::uint32_t cur = to_raw(ent);
        while (parent.count(cur) && d < 64) { cur = parent[cur]; ++d; }
        reg.emplace_or_replace<ecs::Depth>(ent, ecs::Depth{d});
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
