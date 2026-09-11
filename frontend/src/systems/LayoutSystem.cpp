#include "rgv/systems/LayoutSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rgv::systems {
namespace {

std::uint32_t to_raw(entt::entity e) { return static_cast<std::uint32_t>(e); }
std::unordered_map<std::uint32_t, entt::entity> containment_parents(entt::registry& reg);

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

// Concentric placement for the dependency views.
//
// The ring is reach: how much of the repository transitively depends on this node. The
// core -- what everything is ultimately built on -- sits in the middle, and consumers
// end up on the rim, which is the shape an architecture diagram is usually drawn in by
// hand. Direct dependents would be the wrong axis: a package imported by one adapter
// that half the repository sits behind has a direct count of 1, and would be exiled to
// the edge while being the actual core.
//
// Angle is ordered by barycentre, the same idea the layered version used for x, so
// dependency lines run roughly radially instead of chording across the middle. The mean
// is circular -- averaging raw angles puts a node with neighbours either side of zero
// on the far side of the ring.
void LayoutSystem::concentric_place(ecs::World& world) {
    auto&       reg     = world.registry;
    const auto& derived = world.resource<ecs::DerivedState>();
    const auto& reach   = derived.reach;

    struct Item { entt::entity e; float half_x, half_y; };
    std::vector<entt::entity> all;
    for (auto [ent, ref] : reg.view<const ecs::NodeRef>().each()) all.push_back(ent);
    if (all.empty()) { energy_ = 1e9f; return; }

    // -- rings, from reach. Bucketed rather than continuous: the barycentre sweeps need
    // discrete layers to order within, and a ring you can see is the point.
    const int widest = std::max(1, reach.widest());
    // Ring count is bounded by the population as well as by the spread of reach. Eight
    // packages over seven rings puts one node on each, and a ring of one is a point on a
    // line, not a ring -- the whole graph comes out as a single radial spoke.
    const int by_population =
        static_cast<int>(std::lround(std::sqrt(static_cast<float>(all.size()))));
    const int rings = std::clamp(std::min({params_.max_rings, widest + 1, by_population}),
                                 2, params_.max_rings);

    std::vector<std::vector<entt::entity>> layer(static_cast<std::size_t>(rings));
    for (auto e : all) {
        const auto* ref = reg.try_get<ecs::NodeRef>(e);
        const int   r   = ref ? reach.dependents(ref->id) : 0;
        // 0 at the core, rings-1 on the rim. sqrt pulls the middle of the distribution
        // inward: reach is heavily skewed, and a linear map leaves every ring but the
        // outermost nearly empty.
        const float t   = 1.0f - std::sqrt(static_cast<float>(r) / static_cast<float>(widest));
        int         idx = static_cast<int>(std::lround(t * static_cast<float>(rings - 1)));
        layer[static_cast<std::size_t>(std::clamp(idx, 0, rings - 1))].push_back(e);
    }

    // Reach is heavily skewed -- a core everything imports, a wide middle, a rim of
    // leaves -- so bucketing it leaves gaps: 63, 62, 5, 0 lands on rings 0, 0, 4, 6 and
    // the three empty ones in between are just a moat. Compacting keeps the order and
    // the grouping and drops the holes, so the rings that exist are the ones you see.
    layer.erase(std::remove_if(layer.begin(), layer.end(),
                               [](const auto& r) { return r.empty(); }),
                layer.end());
    if (layer.empty()) { energy_ = 1e9f; return; }

    // Deterministic seed order, so the same graph always lays out the same way.
    for (auto& row : layer) {
        std::sort(row.begin(), row.end(), [&](entt::entity a, entt::entity b) {
            const auto* la = reg.try_get<ecs::Label>(a);
            const auto* lb = reg.try_get<ecs::Label>(b);
            return (la ? la->text : "") < (lb ? lb->text : "");
        });
    }

    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> adj;
    for (auto [ent, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) continue;
        adj[to_raw(ends.from)].push_back(to_raw(ends.to));
        adj[to_raw(ends.to)].push_back(to_raw(ends.from));
    }

    // -- barycentre sweeps, in angle
    auto angles_of = [&](const std::vector<entt::entity>& row) {
        std::unordered_map<std::uint32_t, float> a;
        const float n = static_cast<float>(std::max<std::size_t>(row.size(), 1));
        for (std::size_t i = 0; i < row.size(); ++i) {
            a[to_raw(row[i])] = 6.2831853f * static_cast<float>(i) / n;
        }
        return a;
    };

    for (int sweep = 0; sweep < params_.sweeps; ++sweep) {
        const bool outward = (sweep % 2) == 0;
        for (std::size_t li = 0; li < layer.size(); ++li) {
            const std::size_t l = outward ? li : layer.size() - 1 - li;
            const std::size_t ref_l =
                outward ? (l == 0 ? 0 : l - 1) : std::min(l + 1, layer.size() - 1);
            if (ref_l == l || layer[l].empty()) continue;

            const auto ref_angle = angles_of(layer[ref_l]);

            std::vector<std::pair<float, entt::entity>> keyed;
            keyed.reserve(layer[l].size());
            for (std::size_t i = 0; i < layer[l].size(); ++i) {
                const entt::entity e = layer[l][i];
                float sx = 0.0f, sy = 0.0f;
                auto  it = adj.find(to_raw(e));
                if (it != adj.end()) {
                    for (auto nb : it->second) {
                        auto f = ref_angle.find(nb);
                        if (f == ref_angle.end()) continue;
                        sx += std::cos(f->second);
                        sy += std::sin(f->second);
                    }
                }
                // No neighbour on the reference ring: hold the angle it already has,
                // rather than collapsing onto zero and dragging the ring around.
                const float own = 6.2831853f * static_cast<float>(i) /
                                  static_cast<float>(std::max<std::size_t>(layer[l].size(), 1));
                const float ang = (sx == 0.0f && sy == 0.0f) ? own : std::atan2(sy, sx);
                keyed.emplace_back(ang < 0.0f ? ang + 6.2831853f : ang, e);
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < keyed.size(); ++i) layer[l][i] = keyed[i].second;
        }
    }

    // -- radii. Each ring has to be long enough round to seat what is on it, and clear
    // of the one inside it. Same constraint the filesystem view's shells solve.
    ring_radius_.clear();
    float prev_r = 0.0f, prev_half = 0.0f;
    for (std::size_t l = 0; l < layer.size(); ++l) {
        const auto& row = layer[l];
        if (row.empty()) { ring_radius_[static_cast<int>(l)] = prev_r; continue; }

        float need = 0.0f, tallest = 0.0f, widest_node = 0.0f;
        for (auto e : row) {
            const auto* ext = reg.try_get<ecs::Extent>(e);
            const float hx  = ext ? ext->half.x : 50.0f;
            const float hy  = ext ? ext->half.y : 16.0f;
            need += hx * 2.0f + params_.node_gap;
            tallest     = std::max(tallest, hy);
            widest_node = std::max(widest_node, hx);
        }

        const float fit   = need / 6.2831853f;                  // circumference -> radius
        const float clear = prev_r + prev_half + tallest + params_.layer_gap;
        // A lone core node sits dead centre; anything else needs room to spread.
        float r = (l == 0 && row.size() == 1) ? 0.0f : std::max(fit, l == 0 ? 0.0f : clear);
        if (l == 0 && row.size() > 1) r = std::max(fit, widest_node);

        ring_radius_[static_cast<int>(l)] = r;
        prev_r    = r;
        prev_half = tallest;
    }

    // -- place, from the core outward so each ring can be turned to face the one inside
    // it. Even spacing decides where nodes sit relative to each other; the offset
    // decides where the whole ring is rotated to. Without it a ring holding a single
    // node always lands at angle zero, and a chain of them draws a straight spoke.
    std::unordered_map<std::uint32_t, float> placed_angle;
    for (std::size_t l = 0; l < layer.size(); ++l) {
        const auto& row = layer[l];
        const float r   = ring_radius_[static_cast<int>(l)];
        const float n   = static_cast<float>(std::max<std::size_t>(row.size(), 1));

        float ox = 0.0f, oy = 0.0f;
        for (std::size_t i = 0; i < row.size(); ++i) {
            float sx = 0.0f, sy = 0.0f;
            auto  it = adj.find(to_raw(row[i]));
            if (it != adj.end()) {
                for (auto nb : it->second) {
                    auto f = placed_angle.find(nb);
                    if (f == placed_angle.end()) continue;
                    sx += std::cos(f->second);
                    sy += std::sin(f->second);
                }
            }
            if (sx == 0.0f && sy == 0.0f) continue;
            // The turn this node would like the ring to make, accumulated circularly.
            const float want = std::atan2(sy, sx);
            const float slot = 6.2831853f * static_cast<float>(i) / n;
            ox += std::cos(want - slot);
            oy += std::sin(want - slot);
        }
        const float offset = (ox == 0.0f && oy == 0.0f) ? 0.0f : std::atan2(oy, ox);

        for (std::size_t i = 0; i < row.size(); ++i) {
            const entt::entity e = row[i];
            const float ang = 6.2831853f * static_cast<float>(i) / n + offset;
            placed_angle[to_raw(e)] = ang;
            const Vec2  target{std::cos(ang) * r, std::sin(ang) * r};

            reg.emplace_or_replace<ecs::Ring>(
                e, ecs::Ring{static_cast<int>(l), r, ang});

            if (reg.all_of<ecs::Pinned>(e)) {
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
    for (const auto& [child, par] : tree_parents(world)) {
        kids[to_raw(par)].push_back(static_cast<entt::entity>(child));
        parent[child] = to_raw(par);
    }

    // What orbits rather than holding an orbit of its own. Containment says a file;
    // an import tree has no such kind, so it says whatever has nothing beneath it.
    const bool by_imports =
        world.resource<ecs::ViewSettings>().mode == ecs::ViewMode::Architecture;
    auto is_file = [&](entt::entity e) {
        if (by_imports) {
            auto it = kids.find(to_raw(e));
            return it == kids.end() || it->second.empty();
        }
        const auto* ref = reg.try_get<ecs::NodeRef>(e);
        return ref && ref->kind == NodeKind::File;
    };
    auto label_of = [&](entt::entity e) {
        const auto* l = reg.try_get<ecs::Label>(e);
        return l ? l->text : std::string{};
    };
    auto id_of = [&](entt::entity e) {
        const auto* ref = reg.try_get<ecs::NodeRef>(e);
        return ref ? ref->id : std::string{};
    };

    // Stable ordering, so the same repository always draws the same way. The id breaks
    // ties, because two files in different directories can share a name.
    for (auto& [k, v] : kids) {
        std::sort(v.begin(), v.end(), [&](entt::entity a, entt::entity b) {
            const bool fa = is_file(a), fb = is_file(b);
            if (fa != fb) return !fa;                 // branches first, then leaves
            const std::string la = label_of(a), lb = label_of(b);
            return la != lb ? la < lb : id_of(a) < id_of(b);
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

    // A directory grows from phi to phi-squared times a file, by the square root of its
    // file count -- so the difference is visible early and then flattens, rather than a
    // handful of huge directories swamping everything else.
    auto draw_radius = [&](std::size_t files) {
        if (files == 0) return params_.dir_radius;
        const float span = params_.dir_radius_max - params_.dir_radius;
        const float t    = std::min(1.0f, std::sqrt(static_cast<float>(files)) / 7.0f);
        return params_.dir_radius + span * t;
    };

    // Explicit stack rather than recursion: a vendored dependency tree gets deep.
    std::unordered_map<std::uint32_t, Vec2>  outward;
    std::unordered_map<std::uint32_t, float> halo;

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

                // Files fill concentric orbits rather than one enormous ring.
                //
                // A single orbit sized to seat every file puts 57 icons on a circle of
                // radius 160 around a disc of radius 16 -- a vast empty annulus. Gource
                // packs children onto successive orbits for exactly this reason, and it
                // is what keeps a wide directory compact enough to read.
                float hull = draw;
                if (files > 0) {
                    std::vector<entt::entity> to_place;
                    to_place.reserve(files);
                    for (auto c : ch) {
                        if (is_file(c)) to_place.push_back(c);
                    }

                    const float per  = 2.0f * params_.file_radius + params_.file_gap;
                    const float base = phase_of(f.node);
                    float       r    = draw + params_.file_radius + params_.orbit_gap;
                    float       last = r;

                    std::size_t placed = 0;
                    int         orbit  = 0;
                    while (placed < to_place.size()) {
                        // How many fit on this orbit without crowding, and never zero.
                        const auto seats = static_cast<std::size_t>(
                            std::max(1.0f, std::floor(6.2831853f * r / per)));
                        const std::size_t n = std::min(seats, to_place.size() - placed);

                        // Each orbit is rotated off the last so the dots interleave
                        // instead of forming radial spokes.
                        const float spin = base + 0.5f * static_cast<float>(orbit++);
                        for (std::size_t k = 0; k < n; ++k) {
                            const float a = spin + 6.2831853f * static_cast<float>(k) /
                                                       static_cast<float>(n);
                            const Vec2  u{std::cos(a), std::sin(a)};
                            out.nodes.push_back({to_place[placed + k], u * r});
                            outward[to_raw(to_place[placed + k])] = u;
                        }
                        placed += n;
                        last = r;
                        r += per;
                    }
                    hull = last + params_.file_radius;
                }
                halo[to_raw(f.node)] = hull;
                out.radius           = hull;

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
                        // Only the subtree root moves relative to this parent; the rest
                        // already have an outward direction from their own.
                        outward[to_raw(subs[k].nodes.front().first)] =
                            Vec2{std::cos(mid), std::sin(mid)};
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
            auto        dir    = outward.find(to_raw(e));
            auto        h      = halo.find(to_raw(e));
            reg.emplace_or_replace<ecs::Disc>(
                e, ecs::Disc{radius, h == halo.end() ? radius : h->second,
                             dir == outward.end() ? Vec2{0.0f, 1.0f} : dir->second});
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

// How much room each node has before it meets a neighbour. Measured after placement, on
// a uniform grid so it stays linear, and used both to cap how far a node may morph
// toward its label box and to decide whether there is space beside it for its name.
void LayoutSystem::measure_spacing(ecs::World& world) {
    auto& reg = world.registry;

    float widest = 40.0f;
    for (auto [e, ext] : reg.view<const ecs::Extent>().each()) {
        widest = std::max(widest, std::max(ext.half.x, ext.half.y));
    }
    const float cell = widest * 2.0f;

    std::unordered_map<std::int64_t, std::vector<entt::entity>> bins;
    auto key = [](int x, int y) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
    };
    for (auto [e, t] : reg.view<const ecs::LayoutTarget>().each()) {
        bins[key(static_cast<int>(std::floor(t.p.x / cell)),
                 static_cast<int>(std::floor(t.p.y / cell)))]
            .push_back(e);
    }

    for (auto [e, t] : reg.view<const ecs::LayoutTarget>().each()) {
        const int cx = static_cast<int>(std::floor(t.p.x / cell));
        const int cy = static_cast<int>(std::floor(t.p.y / cell));
        float     best = cell * 2.0f;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                auto it = bins.find(key(cx + dx, cy + dy));
                if (it == bins.end()) continue;
                for (auto other : it->second) {
                    if (other == e) continue;
                    const auto* ot = reg.try_get<ecs::LayoutTarget>(other);
                    if (!ot) continue;
                    best = std::min(best, length(ot->p - t.p));
                }
            }
        }
        reg.emplace_or_replace<ecs::Spacing>(e, ecs::Spacing{best * 0.5f});
    }
}

void LayoutSystem::reset(ecs::World& world) {
    const auto mode = world.resource<ecs::ViewSettings>().mode;
    // Two views are trees now: the filesystem by containment, the architecture by
    // imports. Same algorithm, different hierarchy.
    tree_mode_ = mode == ecs::ViewMode::Filesystem || mode == ecs::ViewMode::Architecture;
    if (tree_mode_) {
        radial_tree(world);
    } else {
        assign_depths(world);
        concentric_place(world);
    }
    // Every layout, so every view can answer the same questions about crowding.
    measure_spacing(world);
}

namespace {

// Parent of each node, from the containment edges the filesystem view synthesises.
std::unordered_map<std::uint32_t, entt::entity> containment_parents(entt::registry& reg) {
    std::unordered_map<std::uint32_t, entt::entity> parent;
    for (auto [e, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) parent[to_raw(ends.from)] = ends.to;
    }
    return parent;
}

} // namespace

// The distance the structural packing gave every containment edge. Captured when a drag
// begins, so the springs relax toward the arrangement the layout produced rather than
// toward some invented ideal.
// child -> parent for the tree the layout is built on.
//
// Containment in the filesystem view. In the architecture view it is the import graph
// instead: the same radial algorithm, driven by what imports what. Spanned breadth
// first from the modules that import nothing, so the foundation sits at the centre and
// each ring outward is code built on the ring inside it. A module's orbiting children
// are the modules that import it, which makes a disc's size "how much is built on this".
//
// Breadth first is also what makes it a tree at all: an import graph has cycles, and
// visiting each node once turns any back edge into a cross-link the layout ignores.
std::unordered_map<std::uint32_t, entt::entity> LayoutSystem::tree_parents(ecs::World& world) const {
    auto& reg = world.registry;
    if (world.resource<ecs::ViewSettings>().mode != ecs::ViewMode::Architecture) {
        return containment_parents(reg);
    }

    std::unordered_map<std::uint32_t, std::vector<entt::entity>> importers;
    std::unordered_map<std::uint32_t, int>                       imports_out;
    for (auto [e, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind != EdgeKind::Imports) continue;
        importers[to_raw(ends.to)].push_back(ends.from);
        ++imports_out[to_raw(ends.from)];
    }

    auto id_of = [&](entt::entity e) {
        const auto* ref = reg.try_get<ecs::NodeRef>(e);
        return ref ? ref->id : std::string{};
    };
    auto by_id = [&](entt::entity a, entt::entity b) { return id_of(a) < id_of(b); };

    entt::entity              root = entt::null;
    std::vector<entt::entity> all;
    for (auto [e, ref] : reg.view<const ecs::NodeRef>().each()) {
        all.push_back(e);
        if (ref.kind == NodeKind::Repository) root = e;
    }
    std::sort(all.begin(), all.end(), by_id);

    std::unordered_map<std::uint32_t, entt::entity> parent;
    if (root == entt::null) return parent;

    std::deque<entt::entity>         queue;
    std::unordered_set<std::uint32_t> seen{to_raw(root)};
    for (auto e : all) {
        if (e == root || imports_out[to_raw(e)] != 0) continue;
        parent[to_raw(e)] = root;   // imports nothing: the foundation, on the first ring
        seen.insert(to_raw(e));
        queue.push_back(e);
    }
    while (!queue.empty()) {
        const entt::entity cur = queue.front();
        queue.pop_front();
        auto it = importers.find(to_raw(cur));
        if (it == importers.end()) continue;
        std::vector<entt::entity> next = it->second;
        std::sort(next.begin(), next.end(), by_id);
        for (auto imp : next) {
            if (!seen.insert(to_raw(imp)).second) continue;
            parent[to_raw(imp)] = cur;
            queue.push_back(imp);
        }
    }
    // Anything a cycle kept out of the traversal still has to be somewhere.
    for (auto e : all) {
        if (e != root && !seen.count(to_raw(e))) parent[to_raw(e)] = root;
    }
    return parent;
}

void LayoutSystem::capture_rest_lengths(ecs::World& world) {
    auto& reg = world.registry;
    rest_.clear();

    for (const auto& [child, parent] : tree_parents(world)) {
        const auto* pc = reg.try_get<ecs::Position>(static_cast<entt::entity>(child));
        const auto* pp = reg.try_get<ecs::Position>(parent);
        if (pc && pp) rest_[child] = std::max(1.0f, length(pc->p - pp->p));
    }
}

// The dragged node follows the cursor; everything else is left to the springs.
//
// It used to translate the whole subtree rigidly, which moved the files but made the
// cluster behave like a solid object. Pulling only the node the user has hold of lets
// its children trail and settle, which is the point of relaxing at all.
void LayoutSystem::apply_drag(ecs::World& world) {
    auto&       reg  = world.registry;
    const auto& drag = world.resource<ecs::DragState>();
    if (!drag.active || !reg.valid(drag.node)) return;

    auto move = [&](entt::entity e) {
        if (auto* pos = reg.try_get<ecs::Position>(e)) pos->p += drag.delta;
        if (auto* t = reg.try_get<ecs::LayoutTarget>(e)) t->p += drag.delta;
    };

    if (relaxing_) {
        move(drag.node);
        return;
    }

    // No containment to relax -- the layered views have none -- so the drag stays rigid
    // and takes whatever the node contains with it.
    std::unordered_map<std::uint32_t, std::vector<entt::entity>> kids;
    for (auto [e, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) kids[to_raw(ends.to)].push_back(ends.from);
    }
    std::vector<entt::entity> moving{drag.node};
    for (std::size_t i = 0; i < moving.size() && moving.size() < 20000; ++i) {
        for (auto c : kids[to_raw(moving[i])]) moving.push_back(c);
    }
    for (auto e : moving) move(e);
}

// Springs along containment, repulsion between overlapping discs.
//
// Position-based: each pass computes displacements and applies them directly, with no
// velocity. Velocity is what makes a force layout oscillate and drift, and drift is the
// thing this codebase spent a rewrite getting rid of. Without it the graph gives way
// under the drag and comes to rest, rather than jiggling indefinitely.
// Live relaxation for the dependency views, constrained to the rings.
//
// The radial tree relaxation is free in both axes because a containment tree has no
// privileged direction. The concentric layout does: the ring IS the reach reading, and
// a node pulled off its own stops telling the truth about how much of the repository
// sits behind it. So radius is sprung home and only the angle is free -- the polar form
// of springing y home and leaving x alone.
//
// There are no containment springs here to pull a dropped node back, and pulling it
// back to its packed angle would simply undo the drag. So the angle the user chose is
// kept, and the relaxation's whole job is to reopen the arc their drop closed.
void LayoutSystem::relax_rings(ecs::World& world, float dt) {
    auto&       reg  = world.registry;
    const auto& drag = world.resource<ecs::DragState>();

    const entt::entity held = drag.active ? drag.node : entt::null;
    auto pinned_fast = [&](entt::entity e) {
        return e == held || reg.all_of<ecs::Pinned>(e);
    };

    const float step = std::clamp(dt * 60.0f, 0.25f, 2.0f);
    constexpr float kTau = 6.2831853f;

    // Re-read polar coordinates from where the nodes actually are, so a dragged node is
    // separated against its current angle rather than the one the layout gave it.
    struct Polar { entt::entity e; float ang; float r; };
    std::unordered_map<int, std::vector<Polar>> rings;
    for (auto [e, pos, ring] : reg.view<const ecs::Position, ecs::Ring>().each()) {
        float a = std::atan2(pos.p.y, pos.p.x);
        if (a < 0.0f) a += kTau;
        ring.angle = a;
        rings[ring.index].push_back({e, a, length(pos.p)});
    }

    for (int iter = 0; iter < params_.relax_iters; ++iter) {
        for (auto& [idx, row] : rings) {
            auto it = ring_radius_.find(idx);
            if (it == ring_radius_.end() || row.size() < 1) continue;
            const float target_r = it->second;

            std::sort(row.begin(), row.end(),
                      [](const Polar& a, const Polar& b) { return a.ang < b.ang; });

            // -- separation, in arc. Wraps: the last node's neighbour is the first.
            if (row.size() > 1 && target_r > 1.0f) {
                for (std::size_t i = 0; i < row.size(); ++i) {
                    Polar& a = row[i];
                    Polar& b = row[(i + 1) % row.size()];

                    const auto* ea = reg.try_get<ecs::Extent>(a.e);
                    const auto* eb = reg.try_get<ecs::Extent>(b.e);
                    const float want = ((ea ? ea->half.x : 50.0f) + (eb ? eb->half.x : 50.0f) +
                                        params_.node_gap) / target_r;

                    float gap = b.ang - a.ang;
                    if (gap < 0.0f) gap += kTau;
                    if (gap >= want) continue;

                    const float push = (want - gap) * params_.relax_repel * step;
                    const bool  fa = pinned_fast(a.e), fb = pinned_fast(b.e);
                    if (fa && fb) continue;
                    if (fa)      b.ang += push;
                    else if (fb) a.ang -= push;
                    else { a.ang -= push * 0.5f; b.ang += push * 0.5f; }
                }
            }

            // -- the ring itself: radius springs home, so a drop lands back on it
            for (auto& n : row) {
                if (pinned_fast(n.e)) continue;
                n.r += (target_r - n.r) * std::min(1.0f, params_.relax_spring * step);
                auto& p = reg.get<ecs::Position>(n.e);
                p.p = Vec2{std::cos(n.ang) * n.r, std::sin(n.ang) * n.r};
            }
        }
    }

    for (auto [e, ring] : reg.view<ecs::Ring>().each()) {
        if (const auto* p = reg.try_get<ecs::Position>(e)) {
            ring.radius = length(p->p);
            float a     = std::atan2(p->p.y, p->p.x);
            ring.angle  = a < 0.0f ? a + kTau : a;
        }
    }

    relax_motion_ = 0.0f;
    for (auto [e, pos, target] : reg.view<const ecs::Position, ecs::LayoutTarget>().each()) {
        relax_motion_ = std::max(relax_motion_, length(pos.p - target.p));
    }
    for (auto [e, pos, target] : reg.view<const ecs::Position, ecs::LayoutTarget>().each()) {
        target.p = pos.p;
    }
}

void LayoutSystem::relax(ecs::World& world, float dt) {
    auto&       reg  = world.registry;
    const auto& drag = world.resource<ecs::DragState>();

    const auto parent = tree_parents(world);
    if (parent.empty()) return;

    const entt::entity held = drag.active ? drag.node : entt::null;
    auto held_fast = [&](entt::entity e) {
        return e == held || reg.all_of<ecs::Pinned>(e);
    };

    // Grid for repulsion, sized to the largest thing in play so a cell's neighbours are
    // enough. All-pairs would be quadratic at three thousand nodes.
    float widest = 12.0f;
    for (auto [e, d] : reg.view<const ecs::Disc>().each()) widest = std::max(widest, d.radius);
    const float cell = widest * 4.0f;

    const float step = std::clamp(dt * 60.0f, 0.25f, 2.0f);

    for (int iter = 0; iter < params_.relax_iters; ++iter) {
        // -- containment springs: hold the distance the packing chose
        for (const auto& [child_raw, par] : parent) {
            const auto child = static_cast<entt::entity>(child_raw);
            auto*      pc    = reg.try_get<ecs::Position>(child);
            auto*      pp    = reg.try_get<ecs::Position>(par);
            if (!pc || !pp) continue;

            auto it = rest_.find(child_raw);
            if (it == rest_.end()) continue;

            const Vec2  d    = pc->p - pp->p;
            const float dist = length(d);
            if (dist < 1e-4f) continue;

            const float err  = dist - it->second;
            const Vec2  push = normalize(d) * (err * params_.relax_spring * step * 0.5f);

            if (!held_fast(child)) pc->p -= push;
            if (!held_fast(par)) pp->p += push;
        }

        // -- repulsion: nothing may sit inside anything else
        std::unordered_map<std::int64_t, std::vector<entt::entity>> bins;
        auto key = [](int x, int y) {
            return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
        };
        for (auto [e, pos] : reg.view<const ecs::Position>().each()) {
            bins[key(static_cast<int>(std::floor(pos.p.x / cell)),
                     static_cast<int>(std::floor(pos.p.y / cell)))]
                .push_back(e);
        }

        for (auto [e, pos, disc] : reg.view<ecs::Position, const ecs::Disc>().each()) {
            const int cx = static_cast<int>(std::floor(pos.p.x / cell));
            const int cy = static_cast<int>(std::floor(pos.p.y / cell));
            Vec2      shove{0.0f, 0.0f};

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    auto it = bins.find(key(cx + dx, cy + dy));
                    if (it == bins.end()) continue;
                    for (auto other : it->second) {
                        if (other == e) continue;
                        const auto* op = reg.try_get<ecs::Position>(other);
                        const auto* od = reg.try_get<ecs::Disc>(other);
                        if (!op || !od) continue;

                        const Vec2  away = pos.p - op->p;
                        const float dist = length(away);
                        const float want = disc.radius + od->radius + 2.0f;
                        if (dist >= want) continue;
                        if (dist < 1e-4f) { shove.x += 0.5f; continue; }
                        shove += normalize(away) * ((want - dist) * 0.5f);
                    }
                }
            }
            if (!held_fast(e)) pos.p += shove * (params_.relax_repel * step);
        }
    }

    // How far anything still moved this frame. The relaxation runs until this falls
    // quiet rather than for a fixed time, so the graph is allowed to finish.
    relax_motion_ = 0.0f;
    for (auto [e, pos, target] : reg.view<const ecs::Position, ecs::LayoutTarget>().each()) {
        relax_motion_ = std::max(relax_motion_, length(pos.p - target.p));
    }

    // The arrangement the relaxation reaches is the arrangement that is kept: targets
    // follow the relaxed positions rather than dragging everything back to the packing.
    for (auto [e, pos, target] : reg.view<const ecs::Position, ecs::LayoutTarget>().each()) {
        target.p = pos.p;
    }
}

// Seat nodes that have just appeared, without disturbing anything already placed.
//
// A full layout is the honest answer to "where does everything go", but it is the wrong
// answer to "where does THIS go": it moves every node on screen, and a filter the user
// is dragging asks the question many times a second. So a newcomer is dropped where it
// belongs -- on its ring, at the angle of whatever it is connected to -- and the
// relaxation takes it from there, which is the same mechanism a drag already uses.
//
// Returns whether anything was seated, so the caller can start the relaxation.
bool LayoutSystem::seat_newcomers(ecs::World& world) {
    auto& reg = world.registry;

    std::vector<entt::entity> fresh;
    for (auto [e] : reg.view<ecs::Unplaced>().each()) fresh.push_back(e);
    if (fresh.empty()) return false;

    // A containment layout has no meaningful "near": a file belongs on its parent's
    // orbit, and the orbits are packed as a whole. Repacking the tree is cheap and
    // stable, so the tree view keeps taking the full path.
    if (tree_mode_) {
        reset(world);
        return false;
    }
    const auto& reach = world.resource<ecs::DerivedState>().reach;

    // Neighbours first: an arriving node almost always has an edge to something that is
    // already on screen, and that is the only cue worth having.
    std::unordered_map<std::uint32_t, std::vector<entt::entity>> nbrs;
    for (auto [ent, ref, ends] : reg.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains) continue;
        nbrs[to_raw(ends.from)].push_back(ends.to);
        nbrs[to_raw(ends.to)].push_back(ends.from);
    }

    // The rings that exist, so a newcomer joins one rather than inventing its own.
    int inner = 0, outer = 0;
    for (const auto& [idx, r] : ring_radius_) {
        inner = std::min(inner, idx);
        outer = std::max(outer, idx);
    }

    for (auto e : fresh) {
        const auto* ref = reg.try_get<ecs::NodeRef>(e);
        if (!ref) { reg.remove<ecs::Unplaced>(e); continue; }

        // Which ring: the same reach bucket the layout would have given it, expressed
        // against the rings that are actually on screen.
        const int   widest = std::max(1, reach.widest());
        const float t = 1.0f - std::sqrt(static_cast<float>(reach.dependents(ref->id)) /
                                         static_cast<float>(widest));
        const int   span = std::max(0, outer - inner);
        const int   idx  = std::clamp(inner + static_cast<int>(std::lround(
                                          t * static_cast<float>(span))), inner, outer);

        auto rit = ring_radius_.find(idx);
        const float radius = rit == ring_radius_.end() ? 0.0f : rit->second;

        // Which angle: the circular mean of the neighbours already placed, so it lands
        // beside what it relates to instead of somewhere it has to travel from.
        float sx = 0.0f, sy = 0.0f;
        auto  it = nbrs.find(to_raw(e));
        if (it != nbrs.end()) {
            for (auto nb : it->second) {
                if (nb == e || reg.all_of<ecs::Unplaced>(nb)) continue;
                const auto* np = reg.try_get<ecs::Position>(nb);
                if (!np || (np->p.x == 0.0f && np->p.y == 0.0f)) continue;
                const float a = std::atan2(np->p.y, np->p.x);
                sx += std::cos(a);
                sy += std::sin(a);
            }
        }
        // Nothing to go beside: spread on the node's own id rather than piling every
        // orphan onto angle zero.
        float angle;
        if (sx == 0.0f && sy == 0.0f) {
            std::uint32_t h = 2166136261u;
            for (unsigned char c : ref->id) { h ^= c; h *= 16777619u; }
            angle = 6.2831853f * static_cast<float>(h % 4096u) / 4096.0f;
        } else {
            angle = std::atan2(sy, sx);
        }

        const Vec2 at{std::cos(angle) * radius, std::sin(angle) * radius};
        reg.emplace_or_replace<ecs::Ring>(e, ecs::Ring{idx, radius, angle});
        reg.emplace_or_replace<ecs::Position>(e, ecs::Position{at});
        reg.emplace_or_replace<ecs::LayoutTarget>(e, ecs::LayoutTarget{at});
        reg.remove<ecs::Unplaced>(e);
    }
    return true;
}

void LayoutSystem::run(ecs::World& world, const ecs::FrameContext& frame) {
    auto& requests = world.resource<ecs::SceneRequests>();
    if (requests.relayout) {
        reset(world);
        requests.relayout = false;
        world.registry.clear<ecs::Unplaced>();
    } else if ((seat_newcomers(world) | requests.resettle) && !relaxing_ && !tree_mode_) {
        // Let the arrivals settle against what is already there, the same way a drop
        // does. Without this they sit exactly on top of whatever shares their angle.
        relaxing_      = true;
        relax_motion_  = 1e9f;
        relax_elapsed_ = 0.0f;
    }
    requests.resettle = false;

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

    // A drag switches on live relaxation. Releasing does not switch it off -- it runs
    // until the graph is quiet, so a dropped node travels somewhere that belongs
    // instead of being frozen where the cursor happened to leave it.
    const auto& drag = world.resource<ecs::DragState>();
    if (drag.active && !relaxing_) {
        capture_rest_lengths(world);
        relaxing_      = true;
        relax_motion_  = 1e9f;
        relax_elapsed_ = 0.0f;
    }

    if (relaxing_) {
        relax_elapsed_ += frame.dt;
        apply_drag(world);
        if (tree_mode_) relax(world, frame.dt);
        else relax_rings(world, frame.dt);

        // Held open while the cursor is down; afterwards it ends when the motion dies
        // away, with a hard cap so a pathological graph cannot relax forever.
        if (!drag.active &&
            (relax_motion_ < params_.relax_quiet || relax_elapsed_ > params_.relax_max)) {
            relaxing_ = false;
        }

        stats.layout_energy  = relax_motion_;
        stats.layout_settled = !relaxing_;
        return;
    }

    apply_drag(world);

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
