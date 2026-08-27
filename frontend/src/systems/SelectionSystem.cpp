#include "rgv/systems/SelectionSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/model/GraphStore.h"

#include <algorithm>

namespace rgv::systems {

void SelectionSystem::run(ecs::World& world, const ecs::FrameContext&) {
    auto&       registry  = world.registry;
    auto&       selection = world.resource<ecs::Selection>();
    const auto& index     = world.resource<ecs::EntityIndex>();
    const auto& store     = world.resource<GraphStore>();
    const auto& view      = world.resource<ecs::ViewSettings>();

    registry.clear<ecs::Selected>();
    registry.clear<ecs::Hovered>();
    registry.clear<ecs::OnExplainedPath>();

    // A node can be selected and then destroyed by an event. Keep the id -- the
    // inspector says so explicitly, because the disappearance is itself information --
    // but mark nothing.
    if (const entt::entity e = index.node(selection.node); e != entt::null) {
        registry.emplace<ecs::Selected>(e);
    }
    if (const entt::entity e = index.edge(selection.edge); e != entt::null) {
        registry.emplace<ecs::Selected>(e);
    }
    if (const entt::entity e = index.node(selection.hovered); e != entt::null) {
        registry.emplace<ecs::Hovered>(e);
    }

    if (selection.node.empty()) {
        selection.path_index = 0;
        return;
    }

    const ImpactResult* result = store.impact(view.level);
    if (!result) return;

    const ImpactedNode* target = nullptr;
    for (const auto& in : result->impacted_nodes) {
        if (in.node_id == selection.node) { target = &in; break; }
    }
    if (!target || target->paths.empty()) {
        selection.path_index = 0;
        return;
    }

    // Wrapped here, because this is the only place that knows how many explanations
    // exist. CyclePath just adds a delta and lets it land.
    const int count = static_cast<int>(target->paths.size());
    selection.path_index = ((selection.path_index % count) + count) % count;

    int hop = 0;
    if (const entt::entity e = index.node(selection.node); e != entt::null) {
        registry.emplace_or_replace<ecs::OnExplainedPath>(e, ecs::OnExplainedPath{hop});
    }
    for (const auto& eid : target->paths[selection.path_index].edges) {
        ++hop;
        if (const entt::entity e = index.edge(eid); e != entt::null) {
            registry.emplace_or_replace<ecs::OnExplainedPath>(e, ecs::OnExplainedPath{hop});
        }
        if (const Edge* edge = store.edge(eid)) {
            if (const entt::entity e = index.node(edge->to); e != entt::null) {
                registry.emplace_or_replace<ecs::OnExplainedPath>(e, ecs::OnExplainedPath{hop});
            }
        }
    }
}

} // namespace rgv::systems
