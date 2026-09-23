#include "rgv/systems/CycleSystem.h"

#include "rgv/analysis/Cycles.h"
#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace rgv::systems {

void CycleSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&       registry = world.registry;
    const auto& index    = world.resource<ecs::EntityIndex>();
    auto&       report   = world.resource<ecs::CycleReport>();

    // Entanglement is a property of which edges exist, and of nothing else that moves
    // per frame. A full SCC plus three maps, sixty times a second, to reach the answer
    // it already had. The marks must never be STALE -- an untangled cycle has to stop
    // being reported in the frame it goes -- and the revision is exactly the signal for
    // that, because untangling means an edge entity was dropped.
    if (primed_ && index.revision == last_revision_) return;
    primed_        = true;
    last_revision_ = index.revision;

    // Cleared and rebuilt whole rather than diffed. Diffing would mean deciding which
    // marks to retire, and getting that wrong leaves the view accusing code that was
    // fixed three saves ago.
    registry.clear<ecs::InCycle>();
    report.groups.clear();
    report.lines = 0;

    // Dependencies only. Containment is drawn as the tree in the filesystem view and
    // as nesting in the architecture view; reading it as a dependency would make every
    // parent and child a two-node cycle.
    std::vector<std::pair<NodeId, NodeId>> edges;
    auto id_of = [&](entt::entity e) -> NodeId {
        const auto* ref = registry.try_get<ecs::NodeRef>(e);
        return ref ? ref->id : NodeId{};
    };
    for (auto [e, ref, ends] : registry.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains || ref.kind == EdgeKind::Owns) continue;
        const NodeId from = id_of(ends.from);
        const NodeId to   = id_of(ends.to);
        if (from.empty() || to.empty()) continue;
        edges.push_back({from, to});
    }

    report.groups = analysis::find_cycles(edges);
    if (report.groups.empty()) return;

    std::unordered_map<NodeId, int> group_of;
    for (std::size_t g = 0; g < report.groups.size(); ++g) {
        for (const auto& id : report.groups[g].nodes) group_of[id] = static_cast<int>(g);
    }

    for (const auto& [id, group] : group_of) {
        const entt::entity ent = index.node(id);
        if (ent != entt::null) registry.emplace_or_replace<ecs::InCycle>(ent, ecs::InCycle{group});
    }

    // A line is part of the finding only when both its ends are in the same group.
    // One that merely arrives at an entangled pair from outside is an ordinary
    // dependency on something tangled, which is a different and much weaker claim.
    for (auto [e, ref, ends] : registry.view<const ecs::EdgeRef, const ecs::Endpoints>().each()) {
        if (ref.kind == EdgeKind::Contains || ref.kind == EdgeKind::Owns) continue;
        auto from = group_of.find(id_of(ends.from));
        auto to   = group_of.find(id_of(ends.to));
        if (from == group_of.end() || to == group_of.end() || from->second != to->second) continue;
        registry.emplace_or_replace<ecs::InCycle>(e, ecs::InCycle{from->second});
        ++report.lines;
    }
}

} // namespace rgv::systems
