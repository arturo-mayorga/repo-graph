// Reconciles the graph store into entities.
//
// The only system that creates or destroys entities. It consumes the store's dirty
// sets, so an ordinary file save touches a handful of entities rather than rebuilding
// the world -- which is what "a file save must not trigger a full global layout"
// (spec 11.2) actually means in practice.
#pragma once

#include "rgv/ecs/Resources.h"
#include "rgv/ecs/System.h"

namespace rgv::systems {

class SceneSyncSystem final : public ecs::System {
public:
    std::string_view name() const override { return "SceneSyncSystem"; }
    void             setup(ecs::World& world) override;
    void             run(ecs::World& world, const ecs::FrameContext& frame) override;

private:
    void rebuild(ecs::World& world);
    void incremental(ecs::World& world);
    void refresh_extents(ecs::World& world);

    bool node_visible(const ecs::World& world, const Node& n) const;
    bool edge_visible(const ecs::World& world, const Edge& e) const;
    void upsert_node(ecs::World& world, const Node& n);
    void upsert_edge(ecs::World& world, const Edge& e);
    void drop_node(ecs::World& world, const NodeId& id);
    void drop_edge(ecs::World& world, const EdgeId& id);
    void seed_position(ecs::World& world, entt::entity e, const Node& n);

    ecs::ViewMode built_mode_ = ecs::ViewMode::Architecture;
    bool          primed_     = false;
};

} // namespace rgv::systems
