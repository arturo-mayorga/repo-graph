#include "rgv/view/HoverLinks.h"

#include <algorithm>
#include <map>

namespace rgv::view {
namespace {

int specificity(EdgeKind k) {
    switch (k) {
        case EdgeKind::Calls:      return 3;
        case EdgeKind::References: return 2;
        case EdgeKind::Imports:    return 1;
        default:                   return 0;
    }
}

bool inside(const GraphStore& store, const NodeId& container, NodeId of) {
    if (container == of) return true;
    for (int guard = 0; guard < 64; ++guard) {
        const Node* n = store.node(of);
        if (!n || n->parent.empty()) return false;
        if (n->parent == container) return true;
        of = n->parent;
    }
    return false;
}

} // namespace

std::vector<HoverLink> hover_links(const GraphStore& store, const NodeId& hovered,
                                   const std::function<bool(const NodeId&)>& on_screen) {
    std::vector<HoverLink> out;
    if (hovered.empty() || !store.node(hovered)) return out;

    auto drawn = [&](NodeId id) -> NodeId {
        for (int guard = 0; guard < 64 && !id.empty(); ++guard) {
            if (on_screen(id)) return id;
            const Node* n = store.node(id);
            if (!n) return {};
            id = n->parent;
        }
        return {};
    };

    std::map<std::pair<NodeId, bool>, HoverLink> best;
    for (const auto& [id, e] : store.edges()) {
        if (!e.active() || !is_dependency_edge(e.kind)) continue;

        const bool from_in = inside(store, hovered, e.from);
        const bool to_in   = inside(store, hovered, e.to);
        if (from_in == to_in) continue;   // wholly inside, or nothing to do with it

        const NodeId far = drawn(from_in ? e.to : e.from);
        if (far.empty() || inside(store, hovered, far)) continue;

        const HoverLink link{far, from_in, e.kind, id};
        auto [it, fresh] = best.try_emplace({far, from_in}, link);
        if (!fresh && specificity(link.kind) > specificity(it->second.kind)) it->second = link;
    }

    out.reserve(best.size());
    for (auto& [key, link] : best) out.push_back(std::move(link));
    return out;
}

std::vector<Vec2> sample_bow(Vec2 a, Vec2 b, Vec2 toward, float bend, int segments,
                             float max_span) {
    segments = std::max(segments, 1);
    const Vec2  mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
    const Vec2  chord{b.x - a.x, b.y - a.y};
    const float span = length(chord);

    // How far to push the control point off the straight line, and in which direction.
    // Toward the hub, but never further than a fraction of the span -- a bow wider than
    // the gap it spans reads as a stray arc rather than as a link between two things.
    Vec2        away{toward.x - mid.x, toward.y - mid.y};
    const float reach = length(away);
    float       push  = 0.0f;
    if (reach < 1e-4f) {
        // The hub is on the line, so there is no "toward". Bow to one side instead, by
        // the span alone, so the curve is still a curve and cannot be read as one of
        // the straight containment stubs it is drawn among.
        away = span > 1e-4f ? Vec2{-chord.y / span, chord.x / span} : Vec2{0.0f, 1.0f};
        push = max_span * span * 0.5f;
    } else {
        away = Vec2{away.x / reach, away.y / reach};
        push = std::min(bend * reach, max_span * span);
    }
    const Vec2  ctrl{mid.x + away.x * push, mid.y + away.y * push};

    std::vector<Vec2> pts;
    pts.reserve(static_cast<std::size_t>(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float u = 1.0f - t;
        pts.push_back(Vec2{u * u * a.x + 2.0f * u * t * ctrl.x + t * t * b.x,
                           u * u * a.y + 2.0f * u * t * ctrl.y + t * t * b.y});
    }
    return pts;
}

} // namespace rgv::view
