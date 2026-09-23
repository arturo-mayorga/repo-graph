#include "rgv/systems/FocusSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace rgv::systems {

void FocusSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&       registry = world.registry;
    const auto& index    = world.resource<ecs::EntityIndex>();
    const auto& selected = world.resource<ecs::Selection>().node;

    // Nothing focused: the component goes away entirely rather than being set to some
    // neutral value, so "is anything focused" is a question about presence and every
    // reader answers it the same way.
    registry.clear<ecs::FocusDistance>();
    if (selected.empty()) return;
    const entt::entity focus = index.node(selected);
    if (focus == entt::null) return;

    // Undirected, and containment is not a dependency: a package and the modules inside
    // it are not near each other because one holds the other, they are near each other
    // when one uses the other.
    std::unordered_map<std::uint32_t, std::vector<entt::entity>> adj;
    for (auto [e, ref, ends] : registry.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains || ref.kind == EdgeKind::Owns) continue;
        adj[static_cast<std::uint32_t>(ends.from)].push_back(ends.to);
        adj[static_cast<std::uint32_t>(ends.to)].push_back(ends.from);
    }

    std::unordered_map<std::uint32_t, int> hops{{static_cast<std::uint32_t>(focus), 0}};
    std::deque<entt::entity>               queue{focus};
    while (!queue.empty()) {
        const entt::entity cur = queue.front();
        queue.pop_front();
        const int d  = hops[static_cast<std::uint32_t>(cur)];
        auto      it = adj.find(static_cast<std::uint32_t>(cur));
        if (it == adj.end()) continue;
        for (auto nb : it->second) {
            if (!hops.emplace(static_cast<std::uint32_t>(nb), d + 1).second) continue;
            queue.push_back(nb);
        }
    }

    for (auto [e, ref] : registry.view<const ecs::NodeRef>().each()) {
        auto it = hops.find(static_cast<std::uint32_t>(e));
        registry.emplace_or_replace<ecs::FocusDistance>(
            e, ecs::FocusDistance{it == hops.end() ? ecs::kUnreached : it->second});
    }

    // A line is as far away as its FARTHER end. The near end is often the focus itself,
    // and taking the nearer of the two would light every edge leaving the selection's
    // neighbourhood as though it were part of it -- which is the opposite of saying
    // where the neighbourhood stops.
    for (auto [e, ref, ends] : registry.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        auto      a = hops.find(static_cast<std::uint32_t>(ends.from));
        auto      b = hops.find(static_cast<std::uint32_t>(ends.to));
        const int da = a == hops.end() ? ecs::kUnreached : a->second;
        const int db = b == hops.end() ? ecs::kUnreached : b->second;
        registry.emplace_or_replace<ecs::FocusDistance>(e, ecs::FocusDistance{std::max(da, db)});
    }
}

} // namespace rgv::systems
