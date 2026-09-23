#include "rgv/systems/HoverLinkSystem.h"

#include "rgv/ecs/Components.h"
#include "rgv/ecs/Resources.h"
#include "rgv/model/GraphStore.h"

namespace rgv::systems {

void HoverLinkSystem::run(ecs::World& world, const ecs::FrameContext&) {
    const auto& registry = world.registry;
    const auto& index    = world.resource<ecs::EntityIndex>();
    const auto& store    = world.resource<GraphStore>();
    const auto& view     = world.resource<ecs::ViewSettings>();
    auto&       out      = world.resource<ecs::HoverLinkSet>();

    out.of.clear();
    out.links.clear();

    // Only the containment view draws these. It is legible because it draws containment
    // and nothing else, so dependencies are shown for one node at a time; the dependency
    // views draw their edges outright and have no use for a second channel.
    if (view.mode != ecs::ViewMode::Filesystem) return;

    for (auto [e, ref] : registry.view<const ecs::NodeRef, const ecs::Hovered>().each()) {
        out.of = ref.id;
    }
    if (out.of.empty()) return;

    out.links = view::hover_links(store, out.of, [&](const NodeId& id) {
        return index.node(id) != entt::null;
    });
}

} // namespace rgv::systems
